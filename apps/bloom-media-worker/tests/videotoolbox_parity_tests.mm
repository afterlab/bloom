// The macOS reader against the FFmpeg reader, in one process, on the same files (issue #371).
//
// H.264 and HEVC decoding is defined to the bit, so every frame the macOS reader returns for those
// streams has to equal the FFmpeg reader's frame exactly: same layout, same tags, same bytes. The
// description of every stream has to be equal as well, so a project does not change with the
// reader that opened its footage. ProRes is the one exception the owner accepted (2026-11-11): its
// decoders are allowed to differ in the last bits, and the bound is the named profile below.
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include "ffmpeg_provider.hpp"
#include "videotoolbox_reader.hpp"

#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/media/provider/protocol.hpp>
#include <bloom/media/provider/provider_chain.hpp>
#include <bloom/media/provider/videotoolbox_manifest.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace bloom::media;

int failures = 0;
void check(const bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what.c_str());
        ++failures;
    }
}

// prores-decode-v1: the bound on the difference between the macOS ProRes decoder and the FFmpeg
// one, in the 16-bit samples that cross the worker boundary: one code value of a 10-bit signal.
// The generated fixtures measure at most 48. Used by nothing but this comparison.
constexpr unsigned kProresDecodeV1 = 64;

[[nodiscard]] std::string text(const provider::Rational& value) {
    return std::to_string(value.numerator) + "/" + std::to_string(value.denominator);
}
[[nodiscard]] std::string text(const provider::ColourTags& value) {
    return std::to_string(value.primaries) + "," + std::to_string(value.transfer) + "," +
           std::to_string(value.matrix) + "," + std::to_string(value.range);
}

template <typename Value, typename Describe>
void same(const std::string& file, const char* field, const Value& apple, const Value& reference,
          const Describe& describe) {
    if (!(apple == reference)) {
        std::fprintf(stderr, "FAILED: %s %s: macOS reader %s, FFmpeg reader %s\n", file.c_str(),
                     field, describe(apple).c_str(), describe(reference).c_str());
        ++failures;
    }
}

// A request as the host would send it to the provider whose handshake is `hello`: the capability
// is that provider's own declaration for the role and the stream's codec.
[[nodiscard]] provider::CallRequest request(const provider::Handshake& hello,
                                            const provider::Role role, const std::string& path,
                                            const provider::ProbeResult* probe = nullptr,
                                            const std::uint32_t stream = 0) {
    provider::CallRequest call;
    call.capability.role = role;
    call.source = path;
    call.stream = stream;
    if (probe != nullptr && role != provider::Role::DemuxIndex) {
        if (const auto* found =
                provider::findDecodeDeclarationV1(hello, role, probe->streams[stream].codec))
            call.capability = found->capability;
        call.width = probe->streams[stream].width;
        call.height = probe->streams[stream].height;
    } else {
        for (const auto& declaration : hello.declarations)
            if (declaration.capability.role == role) {
                call.capability = declaration.capability;
                break;
            }
    }
    return call;
}

// The largest difference between two frames' 16-bit samples, or -1 when they are not comparable.
[[nodiscard]] long sampleDifference(const provider::FrameProduct& left,
                                    const provider::FrameProduct& right) {
    if (left.format != right.format || left.planes.size() != right.planes.size())
        return -1;
    long worst = 0;
    for (std::size_t index = 0; index < left.planes.size(); ++index) {
        const auto& a = left.planes[index];
        const auto& b = right.planes[index];
        if (a.width != b.width || a.height != b.height || a.stride != b.stride ||
            a.bytes.size() != b.bytes.size() || a.bytes.size() % 2U != 0)
            return -1;
        for (std::size_t offset = 0; offset < a.bytes.size(); offset += 2) {
            std::uint16_t x = 0, y = 0;
            std::memcpy(&x, a.bytes.data() + offset, 2);
            std::memcpy(&y, b.bytes.data() + offset, 2);
            worst = std::max(worst, std::labs(static_cast<long>(x) - static_cast<long>(y)));
        }
    }
    return worst;
}

void compare(const std::filesystem::path& path, const bool exact) {
    const auto file = path.filename().string();
    const auto name = path.string();
    videotoolbox::Reader reader;
    const auto appleHello = provider::videoToolboxHandshake(true);
    const auto referenceHello = provider::ffmpegHandshake();
    auto applePayload = reader.call(request(appleHello, provider::Role::Probe, name));
    auto referencePayload = ffmpeg::call(request(referenceHello, provider::Role::Probe, name));
    const auto* apple = std::get_if<provider::ProbeResult>(&applePayload);
    const auto* reference = std::get_if<provider::ProbeResult>(&referencePayload);
    if (apple == nullptr) {
        const auto* error = std::get_if<provider::Unavailable>(&applePayload);
        check(false, file + ": the macOS reader refused it: " + (error ? error->detail : ""));
        return;
    }
    if (reference == nullptr) {
        const auto* error = std::get_if<provider::Unavailable>(&referencePayload);
        check(false, file + ": the FFmpeg reader refused it: " + (error ? error->detail : ""));
        return;
    }

    const auto plain = [](const std::string& value) { return value; };
    const auto number = [](const auto value) { return std::to_string(value); };
    const auto rational = [](const provider::Rational& value) { return text(value); };
    same(file, "container", apple->container, reference->container, plain);
    same(file, "source bytes", apple->sourceBytes, reference->sourceBytes, number);
    check(apple->sourceDigest == reference->sourceDigest, file + ": source digest");
    same(file, "stream count", apple->streams.size(), reference->streams.size(), number);
    if (apple->streams.size() != reference->streams.size())
        return;
    for (std::size_t index = 0; index < apple->streams.size(); ++index) {
        const auto& a = apple->streams[index];
        const auto& r = reference->streams[index];
        const auto label = file + " stream " + std::to_string(index);
        same(label, "id", a.id, r.id, number);
        same(label, "kind", static_cast<int>(a.kind), static_cast<int>(r.kind), number);
        if (a.kind == provider::MediaKind::Data && r.kind == provider::MediaKind::Data)
            continue;
        same(label, "codec", a.codec, r.codec, plain);
        same(label, "profile", a.profile, r.profile, plain);
        same(label, "timebase", a.timebase, r.timebase, rational);
        same(label, "rate", a.rate, r.rate, rational);
        same(label, "width", a.width, r.width, number);
        same(label, "height", a.height, r.height, number);
        same(label, "format", static_cast<int>(a.format), static_cast<int>(r.format), number);
        same(label, "color tags", a.colour, r.colour,
             [](const provider::ColourTags& value) { return text(value); });
        same(label, "duration", a.duration, r.duration, rational);
        same(label, "frame count", a.frameCount, r.frameCount, number);
        same(label, "pixel format", a.pixelFormat, r.pixelFormat, plain);
        same(label, "timecode", a.timecode, r.timecode, plain);
        same(label, "field order", static_cast<int>(a.fieldOrder), static_cast<int>(r.fieldOrder),
             number);
        same(label, "pixel aspect", a.pixelAspect, r.pixelAspect, rational);
        same(label, "sample rate", a.sampleRate, r.sampleRate, number);
        check(a.channelLayout == r.channelLayout, label + ": channel layout");
    }

    auto appleIndexCall = request(appleHello, provider::Role::DemuxIndex, name);
    auto referenceIndexCall = request(referenceHello, provider::Role::DemuxIndex, name);
    appleIndexCall.sourceDigest = referenceIndexCall.sourceDigest = apple->sourceDigest;
    auto appleIndex = reader.call(appleIndexCall);
    auto referenceIndex = ffmpeg::call(referenceIndexCall);
    const auto* appleKeys = std::get_if<provider::DemuxIndex>(&appleIndex);
    const auto* referenceKeys = std::get_if<provider::DemuxIndex>(&referenceIndex);
    // The video entries: the FFmpeg reader also lists every audio and timecode packet, in the
    // groups its demuxer happens to read them in, which is not a property of the file.
    const auto videoKeys = [&](const provider::DemuxIndex* index) {
        std::vector<provider::IndexEntry> entries;
        if (index != nullptr)
            for (const auto& entry : index->keyframes)
                if (entry.stream < apple->streams.size() &&
                    apple->streams[entry.stream].kind == provider::MediaKind::Video)
                    entries.push_back(entry);
        return entries;
    };
    const auto appleEntries = videoKeys(appleKeys), referenceEntries = videoKeys(referenceKeys);
    check(appleKeys != nullptr && referenceKeys != nullptr && !appleEntries.empty() &&
              appleEntries == referenceEntries,
          file + ": the video sync sample index equals the FFmpeg reader's");

    for (std::uint32_t stream = 0; stream < apple->streams.size(); ++stream) {
        const auto& described = apple->streams[stream];
        if (described.kind == provider::MediaKind::Video) {
            // Forward from the start, then out of order: the last frame, back to the middle, and
            // the first again.
            const auto count = described.frameCount;
            std::vector<std::uint64_t> order;
            for (std::uint64_t frame = 0; frame < std::min<std::uint64_t>(count, 16); ++frame)
                order.push_back(frame);
            if (count > 0) {
                order.push_back(count - 1);
                order.push_back(count / 2);
                order.push_back(0);
            }
            long worst = 0;
            for (const auto frame : order) {
                auto call = request(appleHello, provider::Role::VideoDecode, name, apple, stream);
                auto referenceCall =
                    request(referenceHello, provider::Role::VideoDecode, name, reference, stream);
                call.frame = referenceCall.frame = frame;
                call.sourceDigest = referenceCall.sourceDigest = apple->sourceDigest;
                auto a = reader.call(call);
                auto r = ffmpeg::call(referenceCall);
                const auto* appleFrame = std::get_if<provider::FrameProduct>(&a);
                const auto* referenceFrame = std::get_if<provider::FrameProduct>(&r);
                const auto label = file + " frame " + std::to_string(frame);
                if (appleFrame == nullptr || referenceFrame == nullptr) {
                    const auto* error = std::get_if<provider::Unavailable>(&a);
                    const auto* other = std::get_if<provider::Unavailable>(&r);
                    check(false, label + ": both readers decode it" +
                                     (error ? " (macOS reader: " + error->detail + ")" : "") +
                                     (other ? " (FFmpeg reader: " + other->detail + ")" : ""));
                    continue;
                }
                same(label, "time", appleFrame->pts, referenceFrame->pts, rational);
                same(label, "color tags", appleFrame->colour, referenceFrame->colour,
                     [](const provider::ColourTags& value) { return text(value); });
                if (exact) {
                    check(*appleFrame == *referenceFrame,
                          label + ": equals the FFmpeg reader's frame bit for bit");
                } else {
                    const auto difference = sampleDifference(*appleFrame, *referenceFrame);
                    check(difference >= 0, label + ": has the FFmpeg reader's layout");
                    worst = std::max(worst, difference);
                }
            }
            if (!exact) {
                std::printf("%s: largest ProRes sample difference %ld of 65535\n", file.c_str(),
                            worst);
                check(worst <= static_cast<long>(kProresDecodeV1),
                      file + ": ProRes decode is within prores-decode-v1");
            }
        } else if (described.kind == provider::MediaKind::Audio) {
            // From the start, from an arbitrary sample, and continuing where that one ended.
            for (const std::uint64_t first :
                 {std::uint64_t{0}, std::uint64_t{1234}, std::uint64_t{1234 + 4800}}) {
                auto call = request(appleHello, provider::Role::AudioDecode, name, apple, stream);
                auto referenceCall =
                    request(referenceHello, provider::Role::AudioDecode, name, reference, stream);
                call.frame = referenceCall.frame = first;
                call.samples = referenceCall.samples = 4800;
                call.sourceDigest = referenceCall.sourceDigest = apple->sourceDigest;
                auto a = reader.call(call);
                auto r = ffmpeg::call(referenceCall);
                const auto* appleBlock = std::get_if<provider::AudioBlock>(&a);
                const auto* referenceBlock = std::get_if<provider::AudioBlock>(&r);
                const auto label = file + " audio from sample " + std::to_string(first);
                if (appleBlock == nullptr || referenceBlock == nullptr) {
                    const auto* error = std::get_if<provider::Unavailable>(&a);
                    const auto* other = std::get_if<provider::Unavailable>(&r);
                    check(false, label + ": both readers decode it" +
                                     (error ? " (macOS reader: " + error->detail + ")" : "") +
                                     (other ? " (FFmpeg reader: " + other->detail + ")" : ""));
                    continue;
                }
                same(label, "time", appleBlock->pts, referenceBlock->pts, rational);
                same(label, "sample rate", appleBlock->sampleRate, referenceBlock->sampleRate,
                     number);
                check(appleBlock->channelLayout == referenceBlock->channelLayout,
                      label + ": channel layout");
                // PCM is the stored samples; nothing is decoded that could differ.
                check(appleBlock->channels == referenceBlock->channels,
                      label + ": equals the FFmpeg reader's samples");
            }
        }
    }
}

// An HEVC file written by the system encoder: the generated fixture set has none.
[[nodiscard]] bool writeHevc(const std::filesystem::path& path, const bool tenBit) {
    @autoreleasepool {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        NSError* error = nil;
        AVAssetWriter* writer = [[AVAssetWriter alloc] initWithURL:url
                                                          fileType:AVFileTypeQuickTimeMovie
                                                             error:&error];
        if (writer == nil)
            return false;
        NSDictionary* settings = @{
            AVVideoCodecKey : AVVideoCodecTypeHEVC,
            AVVideoWidthKey : @128,
            AVVideoHeightKey : @72,
            AVVideoColorPropertiesKey : @{
                AVVideoColorPrimariesKey : AVVideoColorPrimaries_ITU_R_709_2,
                AVVideoTransferFunctionKey : AVVideoTransferFunction_ITU_R_709_2,
                AVVideoYCbCrMatrixKey : AVVideoYCbCrMatrix_ITU_R_709_2,
            },
            AVVideoCompressionPropertiesKey : @{
                AVVideoProfileLevelKey :
                    (__bridge NSString*)(tenBit ? kVTProfileLevel_HEVC_Main10_AutoLevel
                                                : kVTProfileLevel_HEVC_Main_AutoLevel),
                AVVideoAllowFrameReorderingKey : @YES,
                AVVideoMaxKeyFrameIntervalKey : @6,
            },
        };
        AVAssetWriterInput* input =
            [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                               outputSettings:settings];
        const OSType layout = tenBit ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
                                     : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        AVAssetWriterInputPixelBufferAdaptor* adaptor = [AVAssetWriterInputPixelBufferAdaptor
            assetWriterInputPixelBufferAdaptorWithAssetWriterInput:input
                                       sourcePixelBufferAttributes:@{
                                           (NSString*)kCVPixelBufferPixelFormatTypeKey : @(layout),
                                           (NSString*)kCVPixelBufferWidthKey : @128,
                                           (NSString*)kCVPixelBufferHeightKey : @72,
                                       }];
        if (![writer canAddInput:input])
            return false;
        [writer addInput:input];
        if (![writer startWriting])
            return false;
        [writer startSessionAtSourceTime:kCMTimeZero];
        for (int frame = 0; frame < 20; ++frame) {
            CVPixelBufferRef buffer = nullptr;
            if (CVPixelBufferCreate(kCFAllocatorDefault, 128, 72, layout, nullptr, &buffer) !=
                kCVReturnSuccess)
                return false;
            CVPixelBufferLockBaseAddress(buffer, 0);
            for (std::size_t plane = 0; plane < 2; ++plane) {
                auto* base =
                    static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, plane));
                const auto stride = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane);
                const auto rows = CVPixelBufferGetHeightOfPlane(buffer, plane);
                const auto columns =
                    CVPixelBufferGetWidthOfPlane(buffer, plane) * (plane == 0 ? 1U : 2U);
                for (std::size_t y = 0; y < rows; ++y)
                    for (std::size_t x = 0; x < columns; ++x) {
                        // A moving pattern inside the legal range of either depth.
                        const auto code =
                            64U + (x * 5U + y * 3U +
                                   static_cast<std::size_t>(frame) * 7U * (plane + 1U)) %
                                      800U;
                        if (tenBit) {
                            const auto word = static_cast<std::uint16_t>(code << 6U);
                            std::memcpy(base + y * stride + x * 2U, &word, 2);
                        } else {
                            base[y * stride + x] = static_cast<std::uint8_t>(16U + code % 200U);
                        }
                    }
            }
            CVPixelBufferUnlockBaseAddress(buffer, 0);
            while (!input.isReadyForMoreMediaData)
                [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                         beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
            const BOOL appended = [adaptor appendPixelBuffer:buffer
                                        withPresentationTime:CMTimeMake(frame, 24)];
            CVPixelBufferRelease(buffer);
            if (!appended)
                return false;
        }
        [input markAsFinished];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [writer finishWritingWithCompletionHandler:^{
          dispatch_semaphore_signal(done);
        }];
        while (dispatch_semaphore_wait(done, DISPATCH_TIME_NOW) != 0)
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                     beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        return writer.status == AVAssetWriterStatusCompleted;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <fixture directory> <scratch directory>\n", argv[0]);
        return 2;
    }
    const std::filesystem::path fixtures(argv[1]), scratch(argv[2]);
    std::filesystem::create_directories(scratch);
    for (const char* name :
         {"numbered-h264.mp4", "untagged-h264.mp4", "pq2020-h264.mp4", "hlg2020-h264.mp4",
          "bars709-h264.mp4", "bars-untagged-h264.mp4", "long-h264.mp4"})
        compare(fixtures / name, true);
    for (const bool tenBit : {false, true}) {
        const auto path = scratch / (tenBit ? "hevc-main10.mov" : "hevc-main.mov");
        check(writeHevc(path, tenBit), path.filename().string() + ": the system encoder wrote it");
        if (std::filesystem::exists(path))
            compare(path, true);
    }
    for (const char* name : {"numbered-prores.mov", "alpha-prores.mov", "untagged-hd-prores.mov",
                             "uhd8k-prores.mov", "chroma-edge-422.mov"})
        compare(fixtures / name, false);
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
