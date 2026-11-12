#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include "videotoolbox_provider.hpp"
#include "videotoolbox_reader.hpp"

#include <bloom/core/sha256.hpp>
#include <bloom/media/provider/protocol.hpp>
#include <bloom/media/provider/videotoolbox_manifest.hpp>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <vector>
#include <numeric>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

namespace {
int failures = 0;
void check(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

// A real H.264 clip generated with the same framework the provider uses; nothing is checked in.
bool writeFixture(const std::string& path) {
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
        NSError* error = nil;
        AVAssetWriter* writer = [[AVAssetWriter alloc] initWithURL:url
                                                          fileType:AVFileTypeQuickTimeMovie
                                                             error:&error];
        if (writer == nil)
            return false;
        NSDictionary* settings = @{
            AVVideoCodecKey : AVVideoCodecTypeH264,
            AVVideoWidthKey : @(64),
            AVVideoHeightKey : @(48),
        };
        AVAssetWriterInput* input =
            [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo outputSettings:settings];
        input.expectsMediaDataInRealTime = NO;
        NSDictionary* attributes = @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)};
        AVAssetWriterInputPixelBufferAdaptor* adaptor = [[AVAssetWriterInputPixelBufferAdaptor alloc]
            initWithAssetWriterInput:input
          sourcePixelBufferAttributes:attributes];
        if (![writer canAddInput:input])
            return false;
        [writer addInput:input];
        if (![writer startWriting])
            return false;
        [writer startSessionAtSourceTime:kCMTimeZero];
        for (int frame = 0; frame < 3; ++frame) {
            CVPixelBufferRef buffer = nullptr;
            CVPixelBufferCreate(kCFAllocatorDefault, 64, 48, kCVPixelFormatType_32BGRA, nullptr,
                                &buffer);
            CVPixelBufferLockBaseAddress(buffer, 0);
            auto* base = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
            const auto stride = CVPixelBufferGetBytesPerRow(buffer);
            for (int y = 0; y < 48; ++y)
                for (int x = 0; x < 64; ++x) {
                    auto* pixel = base + static_cast<std::size_t>(y) * stride +
                                  static_cast<std::size_t>(x) * 4;
                    pixel[0] = static_cast<std::uint8_t>(x * 4);
                    pixel[1] = static_cast<std::uint8_t>(y * 5);
                    pixel[2] = static_cast<std::uint8_t>(frame * 80);
                    pixel[3] = 255;
                }
            CVPixelBufferUnlockBaseAddress(buffer, 0);
            while (!input.isReadyForMoreMediaData)
                [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                         beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
            [adaptor appendPixelBuffer:buffer withPresentationTime:CMTimeMake(frame, 24)];
            CVPixelBufferRelease(buffer);
        }
        [input markAsFinished];
        dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
        [writer finishWritingWithCompletionHandler:^{
            dispatch_semaphore_signal(semaphore);
        }];
        while (dispatch_semaphore_wait(semaphore, DISPATCH_TIME_NOW) != 0)
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                     beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        return writer.status == AVAssetWriterStatusCompleted;
    }
}

bloom::media::provider::Rational reduced(std::int64_t numerator, std::int64_t denominator) {
    if (numerator == 0)
        return {0, 1};
    const auto divisor = std::gcd(numerator < 0 ? -numerator : numerator, denominator);
    return {numerator / divisor, denominator / divisor};
}

void testProbeAndDecode(const std::string& path) {
    using namespace bloom::media;
    videotoolbox::Reader reader;
    provider::CallRequest probeRequest;
    probeRequest.capability.role = provider::Role::Probe;
    probeRequest.source = path;
    auto probePayload = reader.call(probeRequest);
    const auto* probe = std::get_if<provider::ProbeResult>(&probePayload);
    check(probe != nullptr && !probe->streams.empty(), "probe returns a stream");
    if (probe != nullptr && !probe->streams.empty()) {
        check(probe->streams[0].codec == "h264", "probe reports h264");
        check(probe->streams[0].id == 0, "probe stream ids are zero-based");
        check(probe->streams[0].width == 64 && probe->streams[0].height == 48,
              "probe reports 64x48");
    }

    provider::CallRequest decodeRequest;
    decodeRequest.capability.role = provider::Role::VideoDecode;
    decodeRequest.capability.codec = "h264";
    decodeRequest.source = path;
    decodeRequest.frame = 1;
    auto framePayload = reader.call(decodeRequest);
    const auto* frame = std::get_if<provider::FrameProduct>(&framePayload);
    check(frame != nullptr && frame->planes.size() == 3, "decode returns three planes");
    if (frame != nullptr && !frame->planes.empty()) {
        check(frame->format == provider::PixelFormat::Yuv420p8, "decode returns YUV420P8");
        check(frame->planes.size() == 3, "decode returns three planes");
        check(frame->planes[0].width == 64 && frame->planes[0].height == 48 &&
                  frame->planes[0].stride == 64,
              "decoded luma plane is 64x48");
        check(frame->planes[1].width == 32 && frame->planes[1].height == 24,
              "decoded chroma plane is 32x24");
        check(frame->pts == reduced(1, 24), "decoded frame 1 is at 1/24s");
    }

    // A stream index the file does not have is a typed refusal, never a crash or a wrong frame.
    provider::CallRequest missingStream;
    missingStream.capability.role = provider::Role::VideoDecode;
    missingStream.capability.codec = "h264";
    missingStream.source = path;
    missingStream.stream = 5;
    auto missingPayload = reader.call(missingStream);
    check(std::get_if<provider::Unavailable>(&missingPayload) != nullptr,
          "a nonexistent stream is refused");
}

void testEncodeEdgeCases() {
    using namespace bloom::media;
    // H.264 with odd dimensions is refused up front rather than failing mid-encode.
    provider::EncodeSettingsV1 odd;
    odd.container = "mov";
    odd.videoCodec = "h264";
    odd.profile = "high";
    odd.width = 65;
    odd.height = 49;
    odd.rate = {24, 1};
    odd.frames = 1;
    videotoolbox::Encoder encoder;
    auto beginPayload = encoder.call(provider::MessageKind::EncodeBegin, odd);
    check(std::get_if<provider::Unavailable>(&beginPayload) != nullptr,
          "odd H.264 dimensions are refused with a typed error");
}

void testEncodeRoundTrip() {
    using namespace bloom::media;
    provider::EncodeSettingsV1 settings;
    settings.container = "mov";
    settings.videoCodec = "prores";
    settings.profile = "hq";
    settings.width = 64;
    settings.height = 48;
    settings.rate = {24, 1};
    settings.frames = 3;
    settings.audioCodec = "pcm_s16le";
    settings.sampleRate = 48000;
    settings.channels = 2;
    settings.audioSamples = 6000;

    videotoolbox::Encoder encoder;
    auto beginPayload = encoder.call(provider::MessageKind::EncodeBegin, settings);
    check(!std::get_if<provider::Unavailable>(&beginPayload), "encode begin accepted");
    for (std::uint64_t index = 0; index < settings.frames; ++index) {
        provider::FrameProduct frame;
        frame.format = provider::PixelFormat::Rgba16;
        frame.pts = reduced(static_cast<std::int64_t>(index) * settings.rate.denominator,
                            settings.rate.numerator);
        provider::CpuPlane plane;
        plane.width = 64;
        plane.height = 48;
        plane.stride = 64 * 8;
        plane.bytes.assign(64U * 48U * 8U, std::byte{0});
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 64; ++x) {
                auto* pixel = reinterpret_cast<unsigned char*>(
                    plane.bytes.data() + static_cast<std::size_t>(y) * plane.stride +
                    static_cast<std::size_t>(x) * 8);
                // Half saturated red, half black: the hard chroma edge the ProRes tolerance profile
                // is most likely to stress, plus a green/blue ramp for coverage.
                const bool red = x < 32;
                pixel[0] = red ? 255 : 0;
                pixel[1] = red ? 255 : 0;
                pixel[3] = static_cast<unsigned char>(y * 5);
                pixel[5] = static_cast<unsigned char>(index * 80);
                pixel[7] = 255;
            }
        frame.planes.push_back(std::move(plane));
        auto framePayload = encoder.call(provider::MessageKind::Frame, frame);
        check(!std::get_if<provider::Unavailable>(&framePayload), "encode frame accepted");
    }
    auto audioBlock = provider::AudioBlock{};
    audioBlock.pts = reduced(0, 1);
    audioBlock.sampleRate = settings.sampleRate;
    audioBlock.channelLayout = {"L", "R"};
    audioBlock.channels.assign(2, std::vector<float>(6000, 0.0F));
    for (std::size_t index = 0; index < audioBlock.channels[0].size(); ++index) {
        audioBlock.channels[0][index] = static_cast<float>(index % 100) / 100.0F;
        audioBlock.channels[1][index] = audioBlock.channels[0][index];
    }
    auto audioPayload = encoder.call(provider::MessageKind::Audio, audioBlock);
    check(!std::get_if<provider::Unavailable>(&audioPayload), "encode audio accepted");

    auto qcPayload = encoder.call(provider::MessageKind::EncodeFinish, std::monostate{});
    const auto* qc = std::get_if<provider::EncodeQcV1>(&qcPayload);
    check(qc != nullptr, "encode finish returns QC");
    if (qc != nullptr) {
        check(qc->frames == settings.frames, "reopened frame count matches");
        check(qc->audioSamples == settings.audioSamples, "encoded audio sample count matches");
        check(qc->bytes > 0, "artifact is not empty");
        check(qc->meanError <= 1967, "reopened pixels meet the ProRes mean tolerance");
        check(qc->duration == reduced(3, 24), "reopened duration is 3/24s");
        auto chunkPayload = encoder.call(provider::MessageKind::EncodeRead, std::uint64_t{0});
        const auto* chunk = std::get_if<provider::EncodedChunkV1>(&chunkPayload);
        check(chunk != nullptr && chunk->offset == 0 && !chunk->bytes.empty(),
              "encode read returns the artifact head");
    }
}
// Issue #666. A movie with PCM sound is reopened and its sound decoded: the file has to hold
// exactly the integers Bloom's conversion gives for the audio it was handed (tolerance profile
// `pcm=exact`), and a file or an expectation with one wrong sample is refused.
void testReopenedPcmIsExact(const std::string& codec) {
    using namespace bloom::media;
    const bool wide = codec == "pcm_s24le";
    const auto note = [&codec](const char* text) { return codec + ": " + text; };
    provider::EncodeSettingsV1 settings;
    settings.container = "mov";
    settings.videoCodec = "prores";
    settings.profile = "hq";
    settings.width = 64;
    settings.height = 48;
    settings.rate = {24, 1};
    settings.frames = 3;
    settings.audioCodec = codec;
    settings.sampleRate = 48000;
    settings.channels = 2;
    settings.audioSamples = 6000;

    // Known by hand, not by the function under test.
    check(videotoolbox::pcmSample(0.5F, false) == 16384 &&
              videotoolbox::pcmSample(1.0F, false) == 32767 &&
              videotoolbox::pcmSample(1.5F, false) == 32767 &&
              videotoolbox::pcmSample(-1.0F, false) == -32768 &&
              videotoolbox::pcmSample(-3.0F, false) == -32768 &&
              videotoolbox::pcmSample(0.5F / 32768.0F, false) == 0 &&
              videotoolbox::pcmSample(1.5F / 32768.0F, false) == 2,
          "16-bit conversion rounds to the nearest even and clips");
    check(videotoolbox::pcmSample(0.5F, true) == 0x40000000 &&
              videotoolbox::pcmSample(1.0F, true) == 0x7FFFFF00 &&
              videotoolbox::pcmSample(-1.0F, true) == std::numeric_limits<std::int32_t>::min() &&
              videotoolbox::pcmSample(1.0F / 8388608.0F, true) == 256,
          "24-bit conversion keeps the upper 24 of 32 bits and clips");

    // Two different channels: a slow ramp of exactly representable steps with values past full
    // scale at its ends, and a faster pattern of values that need rounding.
    auto audio = provider::AudioBlock{};
    audio.pts = reduced(0, 1);
    audio.sampleRate = settings.sampleRate;
    audio.channelLayout = {"L", "R"};
    audio.channels.assign(2, std::vector<float>(settings.audioSamples, 0.0F));
    for (std::size_t index = 0; index < settings.audioSamples; ++index) {
        audio.channels[0][index] = (static_cast<float>(index) - 3000.0F) * 11.0F / 32768.0F;
        audio.channels[1][index] =
            static_cast<float>(static_cast<int>((index * 7919U) % 2001U) - 1000) / 997.0F;
    }
    // The stored integers and their digest, written out here: 16 bits as two little-endian
    // bytes, 24 bits as four with the low one zero.
    const auto digestOf = [wide](const provider::AudioBlock& block) {
        bloom::core::Sha256Hasher hasher;
        for (std::size_t frame = 0; frame < block.channels[0].size(); ++frame)
            for (const auto& channel : block.channels) {
                const auto bits =
                    static_cast<std::uint32_t>(videotoolbox::pcmSample(channel[frame], wide));
                const std::array<std::byte, 4> bytes{static_cast<std::byte>(bits & 0xFFU),
                                                     static_cast<std::byte>((bits >> 8U) & 0xFFU),
                                                     static_cast<std::byte>((bits >> 16U) & 0xFFU),
                                                     static_cast<std::byte>((bits >> 24U) & 0xFFU)};
                (void)hasher.update(std::span<const std::byte>(bytes.data(), wide ? 4U : 2U));
            }
        return hasher.finalize();
    };
    const auto expected = digestOf(audio);

    videotoolbox::Encoder encoder;
    auto begin = encoder.call(provider::MessageKind::EncodeBegin, settings);
    check(!std::get_if<provider::Unavailable>(&begin), note("encode begin accepted").c_str());
    for (std::uint64_t index = 0; index < settings.frames; ++index) {
        provider::FrameProduct frame;
        frame.format = provider::PixelFormat::Rgba16;
        frame.pts = reduced(static_cast<std::int64_t>(index) * settings.rate.denominator,
                            settings.rate.numerator);
        provider::CpuPlane plane;
        plane.width = 64;
        plane.height = 48;
        plane.stride = 64 * 8;
        plane.bytes.assign(64U * 48U * 8U, std::byte{0x40});
        frame.planes.push_back(std::move(plane));
        auto accepted = encoder.call(provider::MessageKind::Frame, frame);
        check(!std::get_if<provider::Unavailable>(&accepted), note("frame accepted").c_str());
    }
    auto sound = encoder.call(provider::MessageKind::Audio, audio);
    check(!std::get_if<provider::Unavailable>(&sound), note("audio accepted").c_str());
    auto finished = encoder.call(provider::MessageKind::EncodeFinish, std::monostate{});
    const auto* qc = std::get_if<provider::EncodeQcV1>(&finished);
    if (qc == nullptr) {
        const auto* error = std::get_if<provider::Unavailable>(&finished);
        check(false,
              note(error != nullptr ? error->detail.c_str() : "encode finish failed").c_str());
        return;
    }
    check(qc->audioSamples == settings.audioSamples,
          note("the reopened track holds every sample frame").c_str());
    check(qc->audio == expected,
          note("the reopened samples are exactly the converted audio").c_str());

    // The artifact, as the host would read it.
    std::vector<std::byte> artifact;
    for (;;) {
        auto read = encoder.call(provider::MessageKind::EncodeRead,
                                 static_cast<std::uint64_t>(artifact.size()));
        const auto* chunk = std::get_if<provider::EncodedChunkV1>(&read);
        if (chunk == nullptr || chunk->bytes.empty())
            break;
        artifact.insert(artifact.end(), chunk->bytes.begin(), chunk->bytes.end());
    }
    check(artifact.size() == qc->bytes, note("the whole artifact is read back").c_str());
    const auto write = [](const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        return file.good();
    };
    const auto folder = std::filesystem::temp_directory_path();
    const auto good = folder / ("bloom-vt-pcm-good-" + codec + ".mov");
    const auto wrong = folder / ("bloom-vt-pcm-wrong-" + codec + ".mov");
    const auto refusedWith = [](const provider::Result<videotoolbox::ReopenedPcm>& result,
                                const std::string_view fragment) {
        const auto* error = std::get_if<provider::Unavailable>(&result);
        return error != nullptr && error->detail.find(fragment) != std::string::npos;
    };
    check(write(good, artifact), note("the artifact is written to a file").c_str());
    const auto same = videotoolbox::reopenedPcm(good.string(), 1, settings, expected);
    check(std::holds_alternative<videotoolbox::ReopenedPcm>(same) &&
              std::get<videotoolbox::ReopenedPcm>(same).digest == expected,
          note("the check passes on the artifact as written").c_str());

    // Wrong expectation: the same file checked against audio that differs in one sample.
    auto other = audio;
    other.channels[1][4321] += 0.25F;
    check(refusedWith(videotoolbox::reopenedPcm(good.string(), 1, settings, digestOf(other)),
                      "Reopened PCM samples differ"),
          note("audio that differs in one sample is refused").c_str());

    // Wrong file: one stored sample of the artifact is changed. The samples are stored as they
    // are, so sixteen consecutive sample frames of the audio are found in the file.
    std::vector<std::byte> pattern;
    for (std::size_t frame = 2000; frame < 2016; ++frame)
        for (const auto& channel : audio.channels) {
            const auto bits =
                static_cast<std::uint32_t>(videotoolbox::pcmSample(channel[frame], wide)) >>
                (wide ? 8U : 0U);
            for (std::uint32_t byte = 0; byte < (wide ? 3U : 2U); ++byte)
                pattern.push_back(static_cast<std::byte>((bits >> (8U * byte)) & 0xFFU));
        }
    const auto found =
        std::search(artifact.begin(), artifact.end(), pattern.begin(), pattern.end());
    check(found != artifact.end(), note("the stored samples are found in the artifact").c_str());
    if (found != artifact.end()) {
        auto tampered = artifact;
        const auto offset = static_cast<std::size_t>(found - artifact.begin()) + pattern.size() / 2;
        tampered[offset] ^= std::byte{0x10};
        check(write(wrong, tampered), note("the changed artifact is written").c_str());
        check(refusedWith(videotoolbox::reopenedPcm(wrong.string(), 1, settings, expected),
                          "Reopened PCM samples differ"),
              note("a file with one changed sample is refused").c_str());
    }

    // Wrong length: the file checked as if it should hold one sample frame fewer, or one more.
    auto shorter = settings;
    shorter.audioSamples -= 1;
    check(refusedWith(videotoolbox::reopenedPcm(good.string(), 1, shorter, expected),
                      "sample count differs"),
          note("a track longer than expected is refused").c_str());
    auto longer = settings;
    longer.audioSamples += 1;
    check(refusedWith(videotoolbox::reopenedPcm(good.string(), 1, longer, expected),
                      "sample count differs"),
          note("a track shorter than expected is refused").c_str());
    std::error_code ignored;
    std::filesystem::remove(good, ignored);
    std::filesystem::remove(wrong, ignored);
}

void testRegistryQualification() {
    using namespace bloom::media;
    const auto hello = provider::videoToolboxHandshake();
    provider::CapabilityRegistry registry;
    for (const auto& declaration : hello.declarations)
        check(std::holds_alternative<provider::Digest>(registry.registerProvider(declaration)),
              "provider declaration registers");
    const auto video = std::find_if(
        hello.declarations.begin(), hello.declarations.end(), [](const auto& declaration) {
            return declaration.capability.role == provider::Role::VideoDecode &&
                   declaration.capability.codec == "h264" &&
                   declaration.capability.purpose == provider::Purpose::Preview;
        });
    check(video != hello.declarations.end(), "h264 preview decode is declared");
    if (video != hello.declarations.end()) {
        const auto pipeline = provider::videoToolboxPipeline(*video);
        check(std::holds_alternative<provider::Digest>(registry.qualifyPipeline(pipeline)),
              "h264 preview decode pipeline qualifies");
        check(std::holds_alternative<provider::MediaAttempt>(registry.begin(pipeline)),
              "h264 preview decode pipeline can begin (the import path)");
    }
}
} // namespace

int main() {
    @autoreleasepool {
        const auto path = (std::filesystem::temp_directory_path() / "bloom-vt-provider-test.mov")
                              .string();
        if (writeFixture(path)) {
            check(true, "fixture generated");
        } else {
            check(false, "fixture could not be generated");
        }
        testProbeAndDecode(path);
        testEncodeRoundTrip();
        testReopenedPcmIsExact("pcm_s16le");
        testReopenedPcmIsExact("pcm_s24le");
        testEncodeEdgeCases();
        testRegistryQualification();
        std::filesystem::remove(path);
    }
    if (failures == 0)
        std::printf("videotoolbox provider: all checks passed\n");
    return failures;
}
