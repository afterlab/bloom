#include "program_audio.hpp"
#include "support.hpp"
#include <algorithm>
#include <bloom/media/provider/encode_session.hpp>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/media/provider/media_workers.hpp>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/wait.h>
#include <tuple>
#include <unistd.h>
#include <utility>
using namespace bloom::media::provider;
namespace {
void ok(const std::optional<Unavailable>& e) {
    if (e)
        throw std::runtime_error(e->detail);
}
template <typename T> T get(Result<T> result) {
    if (const auto* e = std::get_if<Unavailable>(&result))
        throw std::runtime_error(e->detail);
    return std::get<T>(std::move(result));
}
// A worker linked against an FFmpeg built without one of the bundled encoders (a dependency prefix
// older than the lock) answers a session with Unavailable "Encoder is not in the intake". That is
// a stale prefix, not a defect in the code under test: the case reports an explicit skip with the
// reason, and any other refusal still fails the test.
bool workerServes(const EncodeSettingsV1& settings, const char* what) {
    EncodeSessionV1 session({});
    const auto refused = session.begin(settings);
    if (refused && refused->reason == Error::Unavailable &&
        refused->detail == "Encoder is not in the intake") {
        std::cout << "SKIP: " << what
                  << " — this FFmpeg has no such encoder (a dependency prefix older than the "
                     "lock; rebuild it)\n";
        return false;
    }
    return true;
}
// The MCP repro: black with a centred 10x10 saturated red square, whose left edge falls on an
// odd column and so shares every 4:2:2/4:2:0 chroma sample with black.
bool redSquare = false;
// Issue #406: a full-width alpha ramp (0 at the left edge, opaque at the right) in place of the
// two-step 4444 alpha, so every 10-bit alpha code the worker converts is exercised.
bool alphaRamp = false;
// Issue #408: an opaque grey ramp from black at the left edge to white at the right, so a 10-bit
// stream carries more distinct luma codes than 8 bits can (bloom.media.hevc-depth reads them).
bool lumaRamp = false;
FrameProduct image(const EncodeSettingsV1& settings, std::uint64_t index) {
    FrameProduct frame;
    frame.format = PixelFormat::Rgba16;
    const auto divisor = std::gcd(index * static_cast<std::uint64_t>(settings.rate.denominator),
                                  static_cast<std::uint64_t>(settings.rate.numerator));
    frame.pts = {static_cast<std::int64_t>(
                     index * static_cast<std::uint64_t>(settings.rate.denominator) / divisor),
                 settings.rate.numerator / static_cast<std::int64_t>(divisor)};
    CpuPlane plane;
    plane.width = settings.width;
    plane.height = settings.height;
    plane.stride = plane.width * 8;
    plane.bytes.resize(static_cast<std::size_t>(plane.stride) * plane.height);
    for (std::uint32_t y = 0; y < plane.height; ++y)
        for (std::uint32_t x = 0; x < plane.width; ++x)
            for (unsigned c = 0; c < 4; ++c) {
                const bool inside = x + 5 >= settings.width / 2 && x < settings.width / 2 + 5 &&
                                    y + 5 >= settings.height / 2 && y < settings.height / 2 + 5;
                const auto ramp = static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(x) * 65535U / std::max(1U, settings.width - 1));
                const auto value = lumaRamp              ? (c == 3 ? 65535U : ramp)
                                   : c == 3 && alphaRamp ? ramp
                                   : c == 3    ? (settings.profile.starts_with("4444") && !redSquare
                                                      ? (x < settings.width / 2 ? 20000U : 50000U)
                                                      : 65535U)
                                   : redSquare ? (inside && c == 0 ? 65535U : 0U)
                                               : (x + index < settings.width / 2 ? 20000U : 40000U);
                const auto offset = static_cast<std::size_t>(y) * plane.stride +
                                    static_cast<std::size_t>(x) * 8U +
                                    static_cast<std::size_t>(c) * 2U;
                plane.bytes[offset] = static_cast<std::byte>(value & 255U);
                plane.bytes[offset + 1] = static_cast<std::byte>(value >> 8U);
            }
    frame.planes.push_back(std::move(plane));
    return frame;
}
AudioBlock sound(const EncodeSettingsV1& settings, std::uint64_t offset, std::uint32_t count) {
    AudioBlock block;
    const auto divisor = std::gcd(offset, static_cast<std::uint64_t>(settings.sampleRate));
    block.pts = {static_cast<std::int64_t>(offset / divisor),
                 static_cast<std::int64_t>(settings.sampleRate / divisor)};
    block.sampleRate = settings.sampleRate;
    block.channelLayout = {"FL", "FR"};
    block.channels.resize(2, std::vector<float>(count));
    for (unsigned i = 0; i < count; ++i)
        for (unsigned c = 0; c < 2; ++c)
            block.channels[c][i] =
                settings.audioCodec == "aac" || settings.audioCodec == "opus"
                    ? static_cast<float>(0.2 * std::sin(static_cast<double>(offset + i + c) * 0.02))
                    : static_cast<float>(static_cast<int>((offset + i + c) % 997) - 498) / 1024.0F;
    return block;
}
// A provider ahead of FFmpeg in the platform's chain that does not declare the codec is passed
// over (ADR 0035); that is not a refusal. These two read provenance past such entries.
bool passedOver(const MediaProviderFallbackV1& fallback) {
    return fallback.cause == MediaFallbackCauseV1::NotDeclared;
}
// The FFmpeg worker served it and nothing was tried before it.
bool servedByFfmpegAlone(const MediaProviderProvenanceV1& served) {
    return served.provider == kFfmpegProviderId &&
           std::ranges::all_of(served.fallbacks, passedOver);
}
// The FFmpeg worker served it after `refused`, the first provider that was tried, gave it up.
bool servedByFfmpegAfter(const MediaProviderProvenanceV1& served, const std::string_view refused) {
    const auto tried = std::ranges::find_if_not(served.fallbacks, passedOver);
    return served.provider == kFfmpegProviderId && tried != served.fallbacks.end() &&
           tried->provider == refused;
}
Bytes run(const EncodeSettingsV1& settings, const std::filesystem::path& destination,
          EncodeSessionOptionsV1 options = {}, MediaProviderProvenanceV1* provenance = nullptr,
          EncodeQcV1* reopened = nullptr) {
    EncodeSessionV1 session(std::move(options));
    ok(session.begin(settings));
    if (provenance != nullptr)
        *provenance = session.provenance();
    for (std::uint64_t i = 0; i < settings.frames; ++i) {
        ok(session.video(image(settings, i)));
        if (!settings.audioCodec.empty())
            ok(session.audio(sound(settings, i * 2000, 2000)));
    }
    if (settings.frames == 0)
        for (std::uint64_t offset = 0; offset < settings.audioSamples; offset += 2000)
            ok(session.audio(sound(settings, offset,
                                   static_cast<std::uint32_t>(std::min<std::uint64_t>(
                                       2000, settings.audioSamples - offset)))));
    const auto qc = get(session.finish());
    if (reopened != nullptr)
        *reopened = qc;
    test::check(qc.frames == settings.frames && qc.audioSamples == settings.audioSamples,
                "reopen counts exact");
    const auto durationNumerator =
        settings.frames ? settings.frames * static_cast<std::uint64_t>(settings.rate.denominator)
                        : settings.audioSamples;
    const auto durationDenominator =
        settings.frames ? static_cast<std::uint64_t>(settings.rate.numerator) : settings.sampleRate;
    const auto divisor = std::gcd(durationNumerator, durationDenominator);
    test::check(qc.duration == Rational{static_cast<std::int64_t>(durationNumerator / divisor),
                                        static_cast<std::int64_t>(durationDenominator / divisor)},
                "reopen duration exact");
    Bytes artifact;
    for (std::uint64_t offset = 0; offset < qc.bytes;) {
        auto chunk = get(session.read(offset));
        offset += chunk.bytes.size();
        artifact.insert(artifact.end(), chunk.bytes.begin(), chunk.bytes.end());
    }
    test::check(digestBytes(artifact) == qc.artifact, "transferred artifact digest");
    ok(session.close());
    std::ofstream file(destination, std::ios::binary);
    file.write(reinterpret_cast<const char*>(artifact.data()),
               static_cast<std::streamsize>(artifact.size()));
    const auto toleranceHex = encodeTolerance(settings).toLowercaseHex();
    // Issue #424: the gated percentile maximum crosses the wire beside the raw maximum and never
    // exceeds it.
    test::check(qc.percentileMaximumError <= qc.maximumError &&
                    qc.percentileMaximumError <= kVideoTolerancePercentileMaximum,
                "reopen percentile maximum within the raw maximum and the v3 bound");
    std::cout << destination.filename() << " frames=" << qc.frames << " samples=" << qc.audioSamples
              << " max=" << qc.maximumError << " p99.99max=" << qc.percentileMaximumError
              << " mean=" << qc.meanError
              << " tolerance=" << std::string(toleranceHex.data(), toleranceHex.size()) << '\n';
    return artifact;
}
void tests(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    EncodeSettingsV1 settings;
    settings.width = 256;
    settings.height = 128;
    settings.frames = 48;
    settings.audioCodec = "pcm_s16le";
    settings.audioSamples = 96000;
    (void)run(settings, directory / "export-prores.mov");
    settings.videoCodec = "dnxhd";
    settings.profile = "dnxhr_hq";
    settings.container = "mxf";
    (void)run(settings, directory / "export-dnxhr.mxf");
    settings.videoCodec.clear();
    settings.profile.clear();
    settings.container = "wav";
    settings.frames = 0;
    const auto wav = run(settings, directory / "export.wav");
    // Independent RIFF/PCM reference, including the muxer's reserved RF64 JUNK chunk.
    Bytes oracle;
    const auto text = [&](std::string_view value) {
        for (const char c : value)
            oracle.push_back(static_cast<std::byte>(c));
    };
    const auto number = [&](std::uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i)
            oracle.push_back(static_cast<std::byte>((value >> (i * 8U)) & 255U));
    };
    text("RIFF");
    number(384072, 4);
    text("WAVEJUNK");
    number(28, 4);
    oracle.resize(48);
    text("fmt ");
    number(16, 4);
    number(1, 2);
    number(2, 2);
    number(48000, 4);
    number(192000, 4);
    number(4, 2);
    number(16, 2);
    text("data");
    number(384000, 4);
    for (std::uint64_t i = 0; i < 96000; ++i)
        for (unsigned c = 0; c < 2; ++c)
            number(static_cast<std::uint16_t>((static_cast<int>((i + c) % 997) - 498) * 32), 2);
    test::check(wav == oracle, "WAV matches independent RIFF/PCM encoder byte for byte");
    test::check(wav == run(settings, directory / "export-repeat.wav"), "WAV whole-file byte exact");
    settings.bwfDescription = "Bloom test reference";
    const auto bwf = run(settings, directory / "export-bwf.wav");
    const std::string bytes(reinterpret_cast<const char*>(bwf.data()), bwf.size());
    test::check(bytes.find("bext") != std::string::npos &&
                    bytes.find(settings.bwfDescription) != std::string::npos,
                "BWF description chunk");
    settings.bwfDescription.clear();
    settings.audioCodec = "pcm_s24le";
    (void)run(settings, directory / "export24.wav");
    settings.audioCodec = "aac";
    settings.container = "mov";
    (void)run(settings, directory / "export-aac.mov");
    {
        // #650: the same tone at twice the level, encoded cleanly. Profile v1 refused it on its
        // per-sample limit; v2 compares levels and the low band, so a correct encode verifies at
        // any level. What v2 refuses is in bloom.media.lossy-audio-compare.
        EncodeSessionV1 louder({});
        ok(louder.begin(settings));
        for (std::uint64_t offset = 0; offset < settings.audioSamples; offset += 2000) {
            auto block = sound(settings, offset, 2000);
            for (auto& channel : block.channels)
                for (auto& sample : channel)
                    sample *= 2;
            ok(louder.audio(std::move(block)));
        }
        const auto qc = get(louder.finish());
        test::check(qc.audioSamples == settings.audioSamples,
                    "a clean AAC encode at twice the level verifies");
        ok(louder.close());
    }
    settings = {};
    settings.width = 256;
    settings.height = 128;
    for (const auto* profile : {"proxy", "lt", "422", "hq", "4444", "4444xq"}) {
        settings.profile = profile;
        (void)run(settings, directory / (std::string(profile) + ".mov"));
    }
    settings.videoCodec = "dnxhd";
    settings.container = "mxf";
    for (const auto* profile : {"dnxhr_lb", "dnxhr_sq", "dnxhr_hq", "dnxhr_hqx", "dnxhr_444"}) {
        settings.profile = profile;
        (void)run(settings, directory / (std::string(profile) + ".mxf"));
    }
    settings.profile = "dnxhd";
    settings.width = 1920;
    settings.height = 1080;
    settings.rate = {25, 1};
    (void)run(settings, directory / "export-dnxhd.mxf");
    settings = {};
    settings.width = 256;
    settings.height = 128;
    settings.frames = 50;
    settings.rate = {25, 1};
    settings.container = "matroska";
    (void)run(settings, directory / "export.mkv");
    settings = {};
    settings.width = 13;
    settings.height = 7;
    settings.videoCodec = "tiff";
    settings.profile = "rgba16";
    settings.container = "tiff";
    (void)run(settings, directory / "export.tiff");
    EncodeSettingsV1 dpxProbe;
    dpxProbe.width = 13;
    dpxProbe.height = 7;
    dpxProbe.videoCodec = "dpx";
    dpxProbe.profile = "rgb10";
    dpxProbe.container = "dpx";
    const bool dpxServed = workerServes(dpxProbe, "DPX sequences");
    if (dpxServed) {
        for (const auto* profile : {"rgb10", "rgb16"}) {
            settings = {};
            settings.width = 13;
            settings.height = 7;
            settings.videoCodec = "dpx";
            settings.profile = profile;
            settings.container = "dpx";
            settings.colour = {1, 8, 0, 2};
            const auto dpx = run(settings, directory / (std::string("export-") + profile + ".dpx"));
            // The header declares the depth, and the reopen check read the tags back.
            test::check(dpx.size() > 1664 && std::to_integer<unsigned>(dpx[0]) == 'X' &&
                            std::to_integer<unsigned>(dpx[803]) ==
                                (std::string_view(profile) == "rgb10" ? 10U : 16U),
                        "DPX header depth");
            // Linear transfer and Rec.709 primaries are the DPX codes 2 and 6.
            test::check(std::to_integer<unsigned>(dpx[801]) == 2U &&
                            std::to_integer<unsigned>(dpx[802]) == 6U,
                        "DPX header colour tags");
        }
    }
    settings = {};
    settings.width = 13;
    settings.height = 7;
    settings.videoCodec = "dpx";
    settings.profile = "rgb12";
    settings.container = "dpx";
    {
        EncodeSessionV1 refused({});
        const auto unavailable = refused.begin(settings);
        test::check(unavailable && unavailable->reason == Error::Unavailable,
                    "DPX profile outside the closed set is unavailable");
    }
    // The platform encoder's part in an encode it declares (owner decision, 2026-11-11: FFmpeg is
    // never substituted for the system ProRes encoder silently).
    {
        EncodeSettingsV1 prores;
        prores.width = 64;
        prores.height = 48;
        // Skipped: the bundled FFmpeg worker encodes, and nothing was passed over or tried.
        EncodeSessionOptionsV1 skipped;
        skipped.platformEncoder = PlatformEncoderV1::Skipped;
        MediaProviderProvenanceV1 served;
        (void)run(prores, directory / "platform-skipped.mov", std::move(skipped), &served);
        test::check(served.provider == kFfmpegProviderId && served.fallbacks.empty(),
                    "a skipped platform encoder leaves the export to the FFmpeg worker");
        EncodeSessionOptionsV1 required;
        required.platformEncoder = PlatformEncoderV1::Required;
#if defined(__APPLE__)
        // Required: an export the system encoder refuses (ProRes is video range only) fails, and
        // says that the refusal is the platform encoder's.
        auto fullRange = prores;
        fullRange.colour.range = 2;
        EncodeSessionV1 session(std::move(required));
        const auto refused = session.begin(fullRange);
        test::check(refused && refused->reason == Error::Unavailable &&
                        session.platformEncoderRefused(),
                    "a required platform encoder that refuses is not replaced by FFmpeg");
        (void)session.close();
#else
        // Without a platform encoder the requirement has nothing to apply to.
        (void)run(prores, directory / "platform-required.mov", std::move(required), &served);
        test::check(served.provider == kFfmpegProviderId,
                    "requiring a platform encoder changes nothing where there is none");
#endif
    }
    // H.264 ships with Bloom (owner, 2026-10-31): the bundled OpenH264 encodes it with nothing
    // installed, in MOV with PCM and in MP4 with AAC.
    settings = {};
    settings.width = 64;
    settings.height = 48;
    settings.frames = 48;
    settings.videoCodec = "h264";
    settings.profile = "high";
    settings.container = "mov";
    settings.audioCodec = "pcm_s16le";
    settings.audioSamples = 96000;
    {
        MediaProviderProvenanceV1 served;
        (void)run(settings, directory / "export-h264.mov", {}, &served);
#if defined(__APPLE__)
        // ADR 0035: the system encoder is ahead of FFmpeg in the macOS chain.
        test::check(served.provider == nativeMediaProviderId() && !served.fellBack(),
                    "H.264 is served by the system encoder on macOS");
#else
        test::check(servedByFfmpegAlone(served),
                    "software H.264 is served by the FFmpeg worker's bundled OpenH264");
#endif
        auto mp4 = settings;
        mp4.container = "mp4";
        mp4.audioCodec = "aac";
        (void)run(mp4, directory / "export-h264.mp4");
        {
            // #650: thirty seconds of music-like audio with drum transients and full-scale clicks
            // peaking at -0.3 dBFS, the kind of program profile v1 refused, exports to H.264
            // MP4 with AAC and verifies. bloom.media.aac-sync reads the published file back
            // through FFmpeg alone and checks its levels and its start against the same samples.
            auto program = mp4;
            program.frames = 720;
            program.audioSamples = bloom::media::testing::kFrames;
            const auto samples = bloom::media::testing::programAudio();
            EncodeSessionV1 session({});
            ok(session.begin(program));
            for (std::uint64_t i = 0; i < program.frames; ++i) {
                ok(session.video(image(program, i)));
                AudioBlock block;
                const std::uint64_t offset = i * 2000;
                const auto divisor =
                    std::gcd(offset, static_cast<std::uint64_t>(program.sampleRate));
                block.pts = {static_cast<std::int64_t>(offset / divisor),
                             static_cast<std::int64_t>(program.sampleRate / divisor)};
                block.sampleRate = program.sampleRate;
                block.channelLayout = {"FL", "FR"};
                block.channels.resize(2, std::vector<float>(2000));
                for (std::size_t frame = 0; frame < 2000; ++frame)
                    for (std::size_t c = 0; c < 2; ++c)
                        block.channels[c][frame] =
                            samples[(static_cast<std::size_t>(i) * 2000 + frame) * 2 + c];
                ok(session.audio(std::move(block)));
            }
            const auto qc = get(session.finish());
            test::check(qc.frames == program.frames && qc.audioSamples == program.audioSamples,
                        "thirty seconds of program audio verifies in H.264 MP4 with AAC");
            std::ofstream file(directory / "export-h264-program.mp4", std::ios::binary);
            for (std::uint64_t offset = 0; offset < qc.bytes;) {
                const auto chunk = get(session.read(offset));
                offset += chunk.bytes.size();
                file.write(reinterpret_cast<const char*>(chunk.bytes.data()),
                           static_cast<std::streamsize>(chunk.bytes.size()));
            }
            ok(session.close());
        }
        // #619: short ranges whose audio is not a whole number of milliseconds keep
        // the exact logical duration in MP4 too.
        for (const std::uint64_t frames : {1U, 2U, 3U, 4U, 5U, 6U, 7U, 12U}) {
            auto brief = mp4;
            brief.frames = frames;
            brief.audioSamples = frames * 2000U;
            (void)run(brief, directory / ("export-h264-" + std::to_string(frames) + ".mp4"));
        }
    }
    // VA-API requested where no render node encodes falls back to OpenH264 without a refusal: the
    // fallback is recorded in provenance, never shown.
    {
        auto absent = settings;
        absent.vaapiDevice = "/dev/dri/renderD-absent";
        EncodeSessionOptionsV1 vaapi;
        vaapi.vaapi = true;
        MediaProviderProvenanceV1 served;
        (void)run(absent, directory / "export-h264-fallback.mov", std::move(vaapi), &served);
#if defined(__APPLE__)
        // There is no VA-API on macOS; the request is served like any other H.264 export.
        test::check(served.provider == nativeMediaProviderId(),
                    "an H.264 export that asks for VA-API is served by the system encoder");
#else
        test::check(servedByFfmpegAfter(served, kFfmpegVaapiProviderId),
                    "H.264 falls back from VA-API to the bundled OpenH264 silently");
#endif
    }
    // HEVC ships with Bloom too: the bundled Kvazaar encodes Main in MOV and MP4 with its 8-bit
    // build and Main 10 with its 10-bit build (#408), the requested profile choosing the build. It
    // pads an extent that is not whole 8x8 blocks (cropped again by the conformance window), and is
    // VA-API's silent fallback.
    {
        auto hevc = settings;
        hevc.videoCodec = "hevc";
        hevc.profile = "main";
        MediaProviderProvenanceV1 served;
        (void)run(hevc, directory / "export-hevc.mov", {}, &served);
        test::check(servedByFfmpegAlone(served),
                    "software HEVC is served by the FFmpeg worker's bundled Kvazaar");
        auto mp4 = hevc;
        mp4.container = "mp4";
        mp4.audioCodec = "aac";
        (void)run(mp4, directory / "export-hevc.mp4");
        EncodeQcV1 delivered;
        (void)run(hevc, directory / "export-hevc-main.mov", {}, nullptr, &delivered);
        test::check(delivered.profile == "Main" && delivered.bitDepth == 8,
                    "an HEVC Main request reports the Main 8-bit stream it delivered");
        // #408: a Main 10 request with no hardware encoder is real Main 10 from the 10-bit
        // Kvazaar, in MOV and MP4, tagged as requested and passing the unchanged reopen check.
        auto main10 = hevc;
        main10.profile = "main10";
        const bool main10Served = workerServes(main10, "HEVC Main 10 (10-bit Kvazaar)");
        for (const auto* container : {"mov", "mp4"}) {
            if (!main10Served)
                break;
            auto request = main10;
            request.container = container;
            request.audioCodec = std::string_view(container) == "mp4" ? "aac" : "pcm_s16le";
            MediaProviderProvenanceV1 tenServed;
            (void)run(request, directory / (std::string("export-hevc-main10.") + container), {},
                      &tenServed, &delivered);
            test::check(servedByFfmpegAlone(tenServed) && delivered.profile == "Main 10" &&
                            delivered.bitDepth == 10 &&
                            delivered.container == std::string(container) + "/hevc" &&
                            delivered.colour == ColourTags{1, 1, 1, 1},
                        "software HEVC Main 10 is delivered as 10-bit Main 10 by the bundled "
                        "10-bit Kvazaar");
        }
        // The same session settings with only the profile changed: the request picks the build.
        (void)run(hevc, directory / "export-hevc-main-again.mov", {}, nullptr, &delivered);
        test::check(delivered.profile == "Main" && delivered.bitDepth == 8,
                    "an HEVC Main request still uses the 8-bit Kvazaar");
        // Grey ramps across 1280 columns for bloom.media.hevc-depth, which counts the distinct
        // decoded luma codes: more than 256 only when the stream really carries 10 bits.
        lumaRamp = true;
        for (const auto* profile : {"main", "main10"}) {
            if (!main10Served && std::string_view(profile) == "main10")
                continue;
            auto ramp = hevc;
            ramp.profile = profile;
            ramp.width = 1280;
            ramp.height = 72;
            ramp.frames = 4;
            ramp.audioCodec.clear();
            ramp.audioSamples = 0;
            (void)run(ramp, directory / ("export-hevc-" + std::string(profile) + "-ramp.mov"));
            ramp.container = "mp4";
            (void)run(ramp, directory / ("export-hevc-" + std::string(profile) + "-ramp.mp4"));
        }
        lumaRamp = false;
        // 1918x1078 is not whole 8x8 blocks: Kvazaar codes the padded picture and crops it with
        // the conformance window, and the reopen check sees the exact extent.
        auto unaligned = hevc;
        unaligned.width = 1918;
        unaligned.height = 1078;
        unaligned.frames = 6;
        unaligned.audioCodec.clear();
        unaligned.audioSamples = 0;
        (void)run(unaligned, directory / "export-hevc-1918x1078.mov");
        unaligned.profile = "main10";
        if (main10Served)
            (void)run(unaligned, directory / "export-hevc-main10-1918x1078.mov");
        auto absent = hevc;
        absent.vaapiDevice = "/dev/dri/renderD-absent";
        EncodeSessionOptionsV1 vaapi;
        vaapi.vaapi = true;
        MediaProviderProvenanceV1 fell;
        (void)run(absent, directory / "export-hevc-fallback.mov", std::move(vaapi), &fell);
        test::check(servedByFfmpegAfter(fell, kFfmpegVaapiProviderId),
                    "HEVC falls back from VA-API to the bundled Kvazaar silently");
        // A Main 10 request falls back to the 10-bit Kvazaar and is still Main 10.
        absent.profile = "main10";
        EncodeSessionOptionsV1 vaapi10;
        vaapi10.vaapi = true;
        if (main10Served) {
            (void)run(absent, directory / "export-hevc-main10-fallback.mov", std::move(vaapi10),
                      &fell, &delivered);
            test::check(servedByFfmpegAfter(fell, kFfmpegVaapiProviderId) &&
                            delivered.profile == "Main 10" && delivered.bitDepth == 10,
                        "HEVC Main 10 falls back from VA-API to the 10-bit Kvazaar as Main 10");
        }
    }
    // ADR 0036: VP9 (libvpx) and AV1 (SVT-AV1) with Opus (libopus) in WebM, and AV1 with AAC in
    // MP4, at 8 and 10 bits, each delivering exactly the profile and depth requested. 64x48 is
    // below SVT-AV1's 64-pixel rate-control floor, which constant quality covers; WebM's
    // millisecond timestamps are checked on that grid. A 1918x1078 extent is odd in 8x8 blocks.
    for (const auto* codec : {"vp9", "av1"})
        for (const auto* depth : {"8bit", "10bit"}) {
            auto web = settings;
            web.videoCodec = codec;
            web.profile = depth;
            web.container = "webm";
            web.audioCodec = "opus";
            web.sampleRate = 48000;
            web.audioSamples = 96000;
            MediaProviderProvenanceV1 served;
            EncodeQcV1 delivered;
            const auto name = std::string("export-") + codec + "-" + depth;
            (void)run(web, directory / (name + ".webm"), {}, &served, &delivered);
            const bool ten = std::string_view(depth) == "10bit";
            test::check(
                servedByFfmpegAlone(served) &&
                    delivered.container == std::string("webm/") + codec &&
                    delivered.bitDepth == (ten ? 10U : 8U) &&
                    delivered.profile == (std::string_view(codec) == "av1" ? "Main"
                                          : ten                            ? "Profile 2"
                                                                           : "Profile 0"),
                "the bundled VP9/AV1 encoder delivers the requested depth in WebM with Opus");
            if (std::string_view(codec) == "av1") {
                auto mp4 = web;
                mp4.container = "mp4";
                mp4.audioCodec = "aac";
                (void)run(mp4, directory / (name + ".mp4"), {}, nullptr, &delivered);
                test::check(delivered.container == "mp4/av1" &&
                                delivered.bitDepth == (ten ? 10U : 8U),
                            "AV1 in MP4 with AAC reopens with its depth");
            }
            auto unaligned = web;
            unaligned.width = 1918;
            unaligned.height = 1078;
            unaligned.frames = 6;
            unaligned.audioCodec.clear();
            unaligned.audioSamples = 0;
            (void)run(unaligned, directory / (name + "-1918x1078.webm"));
        }
    // WebM carries only VP9, AV1 and Opus.
    {
        auto wrong = settings;
        wrong.container = "webm";
        wrong.audioCodec = "opus";
        wrong.sampleRate = 48000;
        EncodeSessionV1 session(EncodeSessionOptionsV1{});
        test::check(session.begin(wrong).has_value(), "H.264 in WebM is refused at begin");
    }
    if (std::filesystem::exists("/dev/dri/renderD128") ||
        std::filesystem::exists("/dev/dri/renderD129")) {
        auto vaapiSettings = settings;
        vaapiSettings.width = 128;
        vaapiSettings.height = 128;
        EncodeSessionOptionsV1 vaapi;
        vaapi.vaapi = true;
        try {
            (void)run(vaapiSettings, directory / "export-h264-vaapi.mov", std::move(vaapi));
        } catch (const std::runtime_error& error) {
            std::cout << "SKIP: VA-API H.264 round trip — driver refused the encode context: "
                      << error.what() << '\n';
        }
    } else {
        std::cout << "SKIP: VA-API H.264 round trip — no render node\n";
    }
    settings = {};
    settings.width = 64;
    settings.height = 48;
    settings.frames = 2;
    bool cancelled = false;
    EncodeSessionV1 stopped({}, [&] { return cancelled; });
    ok(stopped.begin(settings));
    ok(stopped.video(image(settings, 0)));
    cancelled = true;
    const auto cancellation = stopped.video(image(settings, 1));
    test::check(cancellation && cancellation->reason == Error::Cancelled,
                "mid-encode cancellation");
    std::int64_t pid = 0;
    EncodeSessionOptionsV1 options;
    options.launched = [&](auto value) { pid = value; };
    EncodeSessionV1 crashed(options);
    ok(crashed.begin(settings));
    ok(crashed.video(image(settings, 0)));
    test::check(::kill(static_cast<pid_t>(pid), SIGKILL) == 0, "kill encode worker");
    // Acknowledgements are collected lazily: the crash is reported by the frame write if the pipe
    // is already closed, otherwise by the next call that collects acknowledgements.
    auto failure = crashed.video(image(settings, 1));
    if (!failure) {
        const auto finished = crashed.finish();
        if (const auto* error = std::get_if<Unavailable>(&finished))
            failure = *error;
    }
    test::check(failure && failure->reason == Error::Crashed, "typed encode crash");
    test::check(failure && failure->detail.find("signal 9") != std::string::npos,
                "the encode crash names how the worker ended");
    {
        int status = 0;
        errno = 0;
        test::check(::waitpid(static_cast<pid_t>(pid), &status, WNOHANG) == -1 && errno == ECHILD,
                    "the crashed encode worker is reaped");
    }
    EncodeSessionV1 retry({});
    ok(retry.begin(settings));
    ok(retry.video(image(settings, 0)));
    ok(retry.video(image(settings, 1)));
    test::check(get(retry.finish()).frames == 2, "fresh encode retries cleanly");
}
// Every lossy preset reopens a tiny saturated feature within tolerance profile v2: the verifier
// models the codec's own chroma sampling instead of failing what 4:2:2/4:2:0 removes by design.
// Issue #561: a picture with fine moving noise (a hash of position and frame, at most 3% of full
// scale, so the reopen stays inside the tolerance), so the encoder always has detail to spend and
// the rate control, not the picture, sets the file's size.
FrameProduct busyImage(const EncodeSettingsV1& settings, std::uint64_t index) {
    auto frame = image(settings, index);
    auto& plane = frame.planes.front();
    for (std::uint32_t y = 0; y < plane.height; ++y)
        for (std::uint32_t x = 0; x < plane.width; ++x) {
            auto hash =
                (x * 73856093U) ^ (y * 19349663U) ^ (static_cast<std::uint32_t>(index) * 83492791U);
            hash ^= hash >> 13U;
            hash *= 0x5bd1e995U;
            hash ^= hash >> 15U;
            for (unsigned c = 0; c < 3; ++c) {
                const auto base = static_cast<std::uint32_t>(
                                      plane.bytes[static_cast<std::size_t>(y) * plane.stride +
                                                  static_cast<std::size_t>(x) * 8U +
                                                  static_cast<std::size_t>(c) * 2U]) |
                                  (static_cast<std::uint32_t>(
                                       plane.bytes[static_cast<std::size_t>(y) * plane.stride +
                                                   static_cast<std::size_t>(x) * 8U +
                                                   static_cast<std::size_t>(c) * 2U + 1U])
                                   << 8U);
                const auto noise = static_cast<std::int32_t>((hash >> (c * 8U)) & 255U) * 16 - 2040;
                const auto value = static_cast<std::uint32_t>(
                    std::clamp(static_cast<std::int32_t>(base) + noise, 0, 65535));
                const auto offset = static_cast<std::size_t>(y) * plane.stride +
                                    static_cast<std::size_t>(x) * 8U +
                                    static_cast<std::size_t>(c) * 2U;
                plane.bytes[offset] = static_cast<std::byte>(value & 255U);
                plane.bytes[offset + 1] = static_cast<std::byte>(value >> 8U);
            }
        }
    return frame;
}
// The file's average video rate in kb/s, from its size and the duration it reopened to.
double encodedKbps(const EncodeSettingsV1& settings) {
    EncodeSessionV1 session({});
    ok(session.begin(settings));
    for (std::uint64_t i = 0; i < settings.frames; ++i)
        ok(session.video(busyImage(settings, i)));
    const auto qc = get(session.finish());
    ok(session.close());
    return static_cast<double>(qc.bytes) * 8.0 / 1000.0 /
           (static_cast<double>(settings.frames) * static_cast<double>(settings.rate.denominator) /
            static_cast<double>(settings.rate.numerator));
}
// Issue #561: the target bit rate of an H.264 or HEVC export (videoBitrateKbps) sets the file's
// rate: it lands near the target, and a higher target gives a larger file.
void targetBitrate() {
    for (const auto* codec : {"h264", "hevc"}) {
        EncodeSettingsV1 settings;
        settings.width = 320;
        settings.height = 240;
        settings.frames = 120;
        settings.videoCodec = codec;
        settings.profile = std::string_view(codec) == "h264" ? "high" : "main";
        settings.container = "mp4";
        double previous = 0;
        for (const std::uint32_t target : {400U, 700U}) {
            settings.videoBitrateKbps = target;
            const auto kbps = encodedKbps(settings);
            std::cout << codec << " target=" << target << " kb/s file=" << kbps << " kb/s\n";
            test::check(kbps > target * 0.7 && kbps < target * 1.3,
                        "the exported file's rate is within 30% of the target bit rate");
            test::check(kbps > previous, "a higher target gives a higher file rate");
            previous = kbps;
        }
    }
}
void chromaSampling(const std::filesystem::path& directory) {
    redSquare = true;
    EncodeSettingsV1 settings;
    settings.width = 640;
    settings.height = 360;
    settings.frames = 12;
    for (const auto* profile : {"proxy", "lt", "422", "hq", "4444", "4444xq"}) {
        settings.profile = profile;
        (void)run(settings, directory / (std::string("red-square-prores-") + profile + ".mov"));
    }
    settings.videoCodec = "dnxhd";
    settings.container = "mxf";
    for (const auto* profile : {"dnxhr_lb", "dnxhr_sq", "dnxhr_hq", "dnxhr_hqx", "dnxhr_444"}) {
        settings.profile = profile;
        (void)run(settings, directory / (std::string("red-square-") + profile + ".mxf"));
    }
    settings.profile = "dnxhd";
    settings.width = 1920;
    settings.height = 1080;
    settings.rate = {25, 1};
    (void)run(settings, directory / "red-square-dnxhd.mxf");
    settings = {};
    settings.width = 640;
    settings.height = 360;
    settings.frames = 12;
    settings.videoCodec = "h264";
    settings.profile = "high";
    (void)run(settings, directory / "red-square-h264.mov");
    // The bundled Kvazaar at 640x360 and at 1920x1080, where its encoder threads must fit the
    // worker's address-space ceiling.
    {
        auto hevc = settings;
        hevc.videoCodec = "hevc";
        hevc.profile = "main";
        (void)run(hevc, directory / "red-square-hevc.mov");
        hevc.width = 1920;
        hevc.height = 1080;
        (void)run(hevc, directory / "red-square-hevc-1080p.mov");
    }
    // The bundled libvpx and SVT-AV1 at 1920x1080 in 10 bits, inside the same ceiling.
    for (const auto* codec : {"vp9", "av1"}) {
        auto web = settings;
        web.videoCodec = codec;
        web.profile = "10bit";
        web.container = "webm";
        web.width = 1920;
        web.height = 1080;
        (void)run(web, directory / (std::string("red-square-") + codec + "-1080p.webm"));
    }
    if (!std::filesystem::exists("/dev/dri/renderD128") &&
        !std::filesystem::exists("/dev/dri/renderD129")) {
        std::cout << "SKIP: VA-API red square — no render node\n";
        return;
    }
    // Issue #394: 24 frames, six times the worker's four VA-API input surfaces, so a surface that
    // is not returned to the pool fails the upload. The driver refusing the device, its frame
    // context or the encoder skips; an upload failure or a verification failure is a failure.
    settings.frames = 24;
    for (const auto& [codec, profile] :
         {std::pair{"h264", "high"}, std::pair{"hevc", "main"}, std::pair{"hevc", "main10"}}) {
        settings.videoCodec = codec;
        settings.profile = profile;
        EncodeSessionOptionsV1 vaapi;
        vaapi.vaapi = true;
        try {
            (void)run(settings,
                      directory /
                          (std::string("red-square-") + codec + "-" + profile + "-vaapi.mov"),
                      std::move(vaapi));
        } catch (const std::runtime_error& error) {
            const std::string_view detail = error.what();
            if ((!detail.starts_with("VA-API") || detail == "VA-API frame upload failed") &&
                detail != "Encoder refused profile or stream layout" &&
                detail != "Encoder is not in the intake")
                throw;
            std::cout << "SKIP: VA-API " << codec << ' ' << profile
                      << " red square — the driver refused: " << error.what() << '\n';
        }
    }
}
#if defined(__linux__)
// The soft address-space limit the worker was launched with, from /proc.
std::uint64_t addressSpaceLimit(std::int64_t pid) {
    std::ifstream limits("/proc/" + std::to_string(pid) + "/limits");
    std::string line;
    while (std::getline(limits, line))
        if (line.starts_with("Max address space")) {
            std::istringstream fields(line.substr(17));
            std::uint64_t soft = 0;
            fields >> soft;
            return soft;
        }
    return 0;
}
#endif
// Issue #406: ProRes 4444 with soft alpha at 1080p and UHD. Every case failed before the fix:
// swscale's shifted 10-bit alpha failed the reopen check for any ramp or antialiased edge, and
// frame-threaded prores_ks (a frame per encoder thread) outgrew the frame-sized worker address
// space at 64 threads in 1080p and at 16 threads in UHD.
void prores4444Threaded(const std::filesystem::path& directory) {
    // The thread budget: automatic resolves to the hardware threads within FFmpeg's maximum, and
    // the count shrinks only where the frame-sized threads would pass the ceiling.
    test::check(proresEncodeThreads(0, 16, 1920, 1080) == 16 &&
                    proresEncodeThreads(0, 0, 1920, 1080) == 1 &&
                    proresEncodeThreads(0, 256, 1920, 1080) == kEncodeMaximumThreads &&
                    proresEncodeThreads(64, 16, 1920, 1080) == 64 &&
                    proresEncodeThreads(64, 16, 3840, 2160) == 56 &&
                    proresEncodeThreads(0, 64, 7680, 4320) == 12 &&
                    proresEncodeThreads(64, 64, 8192, 8192) == 4,
                "ProRes encoder threads fit the worker address-space ceiling");
    test::check(proresEncodeAddressSpaceBytes(1920, 1080, 1) == (std::uint64_t{9} << 28U) &&
                    proresEncodeAddressSpaceBytes(1920, 1080, 16) == (std::uint64_t{3} << 30U) &&
                    proresEncodeAddressSpaceBytes(1920, 1080, 64) == (std::uint64_t{6} << 30U) &&
                    proresEncodeAddressSpaceBytes(3840, 2160, 16) == (std::uint64_t{6} << 30U) &&
                    proresEncodeAddressSpaceBytes(16384, 16384, 64) == kWorkerAddressSpaceCeiling,
                "ProRes worker address space grows with frame extent and threads");
    alphaRamp = true;
    EncodeSettingsV1 settings;
    settings.rate = {25, 1};
    settings.width = 1920;
    settings.height = 1080;
    settings.frames = 24;
    // The export runner's automatic thread count, in both alpha profiles.
    settings.threads = 0;
    for (const auto* profile : {"4444", "4444xq"}) {
        settings.profile = profile;
        (void)run(settings,
                  directory / (std::string("alpha-ramp-prores-") + profile + "-1080p.mov"));
    }
    // A 64-thread computer's automatic count, and UHD at 16 threads.
    settings.profile = "4444";
    for (const auto& [width, height, threads, frames] :
         {std::tuple{1920U, 1080U, 64U, 70U}, std::tuple{3840U, 2160U, 16U, 20U}}) {
        settings.width = width;
        settings.height = height;
        settings.threads = threads;
        settings.frames = frames;
        // The worker runs with the address space its frame extent and threads need.
        std::uint64_t limit = 0;
        EncodeSessionOptionsV1 options;
        options.launched = [&limit](std::int64_t pid) {
#if defined(__linux__)
            limit = addressSpaceLimit(pid);
#else
            (void)pid;
            (void)limit;
#endif
        };
        (void)run(settings,
                  directory / ("alpha-ramp-prores-4444-" + std::to_string(height) + "p-" +
                               std::to_string(threads) + "t.mov"),
                  std::move(options));
#if defined(__linux__)
        test::check(limit == proresEncodeAddressSpaceBytes(width, height, threads),
                    "the ProRes worker runs with its thread-sized address space");
#endif
    }
    alphaRamp = false;
}
} // namespace
// ADR 0036: NVIDIA NVENC is the first hardware encoder. Run under the machine GPU lock as
// bloom.media.encode-nvenc; without an NVIDIA driver every case is skipped (exit 77), and a driver
// that answers must serve each request itself and pass the unchanged reopen check.
int nvencTests(const std::filesystem::path& directory) {
    EncodeSettingsV1 settings;
    settings.width = 1920;
    settings.height = 1080;
    settings.frames = 24;
    settings.videoCodec = "h264";
    settings.profile = "high";
    settings.container = "mov";
    settings.audioCodec = "pcm_s16le";
    settings.audioSamples = 48000;
    EncodeSessionOptionsV1 options;
    options.nvenc = true;
    MediaProviderProvenanceV1 served;
    EncodeQcV1 delivered;
    (void)run(settings, directory / "nvenc-h264.mov", options, &served, &delivered);
    if (served.provider != kFfmpegNvencProviderId) {
        std::cout << "SKIP: NVENC — no NVIDIA driver opened an encode session (" +
                         served.diagnostic() + ")\n";
        return 77;
    }
    test::check(delivered.profile == "High" && delivered.bitDepth == 8,
                "NVENC H.264 delivers High 8-bit");
    const auto nvenc = [&](EncodeSettingsV1 request, const char* file) {
        MediaProviderProvenanceV1 provenance;
        EncodeQcV1 qc;
        (void)run(request, directory / file, options, &provenance, &qc);
        test::check(provenance.provider == kFfmpegNvencProviderId,
                    "NVENC serves the request itself");
        return qc;
    };
    auto mp4 = settings;
    mp4.container = "mp4";
    mp4.audioCodec = "aac";
    (void)nvenc(mp4, "nvenc-h264.mp4");
    auto hevc = settings;
    hevc.videoCodec = "hevc";
    hevc.profile = "main";
    test::check(nvenc(hevc, "nvenc-hevc.mov").bitDepth == 8, "NVENC HEVC Main is 8-bit");
    hevc.profile = "main10";
    const auto ten = nvenc(hevc, "nvenc-hevc-main10.mov");
    test::check(ten.profile == "Main 10" && ten.bitDepth == 10,
                "NVENC delivers real 10-bit HEVC Main 10");
    hevc.container = "mp4";
    hevc.audioCodec = "aac";
    (void)nvenc(hevc, "nvenc-hevc-main10.mp4");
    // Extents that are not whole blocks round-trip through the reopen check too.
    for (const auto* codec : {"h264", "hevc"}) {
        auto odd = settings;
        odd.videoCodec = codec;
        odd.profile = std::string_view(codec) == "h264" ? "high" : "main";
        odd.width = 1918;
        odd.height = 1078;
        odd.frames = 6;
        odd.audioCodec.clear();
        odd.audioSamples = 0;
        (void)nvenc(odd, (std::string("nvenc-") + codec + "-1918x1078.mov").c_str());
    }
    return 0;
}
// Issue #581: the opt-in worker memory cap and the worker memory report.
void workerMemoryCap() {
    EncodeSettingsV1 settings;
    settings.width = 64;
    settings.height = 48;
    settings.frames = 3;
    constexpr std::uint64_t mib = std::uint64_t{1} << 20U;

    // No cap (the default): nothing is held back, and the worker's memory is still reported.
    {
        EncodeSessionV1 session({});
        ok(session.begin(settings));
        for (std::uint64_t i = 0; i < settings.frames; ++i)
            ok(session.video(image(settings, i)));
        (void)get(session.finish());
        const auto memory = session.workerMemory();
        test::check(memory.capBytes == 0 && memory.waits == 0 && !memory.capReached &&
                        memory.waited.count() == 0,
                    "without a cap nothing waits");
        test::check(!memory.measured || memory.peakResidentBytes > 0,
                    "a measured worker reports a peak");
#if defined(__linux__)
        test::check(memory.measured && memory.peakResidentBytes > mib,
                    "the worker's peak resident memory is measured");
#endif
        ok(session.close());
    }

    // The worker is above the cap with nothing left to finish: the session fails with a sentence
    // that names the memory and the cap, and nothing is sent.
    {
        EncodeSessionOptionsV1 options;
        options.memoryCapBytes = 100 * mib;
        options.memorySettle = std::chrono::milliseconds(0);
        options.memorySampler = [](std::int64_t) {
            return std::optional(bloom::platform::ProcessMemory{300 * mib, 300 * mib});
        };
        EncodeSessionV1 session(options);
        ok(session.begin(settings));
        const auto refused = session.video(image(settings, 0));
        test::check(refused && refused->reason == Error::Busy &&
                        refused->detail.find("300 MiB") != std::string::npos &&
                        refused->detail.find("cap is 100 MiB") != std::string::npos,
                    "a worker that cannot proceed under the cap fails with a clear message");
        const auto memory = session.workerMemory();
        test::check(memory.capReached && memory.capBytes == 100 * mib && memory.waits == 1 &&
                        memory.peakResidentBytes == 300 * mib,
                    "the failure is reported with the cap and the peak");
    }

    // The worker is at the cap but recovers: the session waits (it does not fail), then goes on.
    {
        int samples = 0;
        EncodeSessionOptionsV1 options;
        options.memoryCapBytes = 100 * mib;
        options.memorySettle = std::chrono::seconds(30);
        options.memorySampler = [&samples](std::int64_t) {
            ++samples;
            return std::optional(bloom::platform::ProcessMemory{
                (samples >= 2 && samples <= 4 ? 150 : 50) * mib, 150 * mib});
        };
        EncodeSessionV1 session(options);
        ok(session.begin(settings));
        for (std::uint64_t i = 0; i < settings.frames; ++i)
            ok(session.video(image(settings, i)));
        test::check(get(session.finish()).frames == settings.frames,
                    "an export that waited at the cap still completes");
        const auto memory = session.workerMemory();
        test::check(memory.waits >= 1 && !memory.capReached && memory.capBytes == 100 * mib &&
                        memory.peakResidentBytes == 150 * mib,
                    "waiting at the cap is counted and the peak is the highest sample");
        ok(session.close());
    }

    // Products the worker still holds are finished first: at the cap with acknowledgements
    // outstanding, the session collects one and samples again instead of sending more.
    {
        bool high = false;
        EncodeSessionOptionsV1 options;
        options.memoryCapBytes = 100 * mib;
        options.memorySettle = std::chrono::seconds(30);
        options.memorySampler = [&high](std::int64_t) {
            const auto value = high ? 150 * mib : 50 * mib;
            high = false;
            return std::optional(bloom::platform::ProcessMemory{value, value});
        };
        EncodeSessionV1 session(options);
        ok(session.begin(settings));
        ok(session.video(image(settings, 0)));
        ok(session.video(image(settings, 1)));
        high = true; // the next sample finds the worker at the cap with two products outstanding
        ok(session.video(image(settings, 2)));
        test::check(get(session.finish()).frames == settings.frames,
                    "draining outstanding products brings the worker under the cap");
        const auto memory = session.workerMemory();
        test::check(memory.waits == 1 && !memory.capReached,
                    "one hold at the cap, resolved by draining");
        ok(session.close());
    }
}
int main(int argc, char** argv) {
    try {
        test::check(argc == 2 || (argc == 3 && std::string_view(argv[2]) == "--nvenc"),
                    "test output directory");
        if (argc == 3)
            return nvencTests(argv[1]);
        prores4444Threaded(argv[1]);
        workerMemoryCap();
        tests(argv[1]);
        chromaSampling(argv[1]);
        targetBitrate();
        // Issue #352: one 8K UHD ProRes frame exports and passes reopen quality check.
        EncodeSettingsV1 eightK;
        eightK.width = 7680;
        eightK.height = 4320;
        eightK.frames = 1;
        (void)run(eightK, std::filesystem::path(argv[1]) / "export-8k-prores.mov");
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
