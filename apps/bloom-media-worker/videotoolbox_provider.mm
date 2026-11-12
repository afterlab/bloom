// The macOS encoder: see videotoolbox_provider.hpp. Compiled with automatic reference counting.
#include "videotoolbox_provider.hpp"

#include "videotoolbox_layout.hpp"
#include "videotoolbox_reader.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreFoundation/CoreFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <bloom/core/sha256.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if defined(BLOOM_VIDEOTOOLBOX_LAYOUT_CONVERSION)
// AVFoundation and libavutil both declare a type named AVMediaType (a string and an enumeration).
// The FFmpeg one is renamed for this file; it names no symbol, so nothing else changes.
#define AVMediaType BloomFfmpegMediaType
#include "ffmpeg_reopen_compare.hpp"
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}
#undef AVMediaType
#endif

#if !defined(__APPLE__)
#error "videotoolbox_provider.mm requires macOS"
#endif
#if !__has_feature(objc_arc)
#error "videotoolbox_provider.mm requires automatic reference counting"
#endif

namespace bloom::media::videotoolbox {

#if !defined(BLOOM_VIDEOTOOLBOX_LAYOUT_CONVERSION)

// The encoder hands the system encoder the same Y'CbCr samples the FFmpeg worker would encode and
// checks its output in the stream's declared sampling. Both use the bundled FFmpeg's libswscale,
// so a build without the FFmpeg package has no macOS encoder.
struct Encoder::State final {};
Encoder::Encoder() : state_(std::make_unique<State>()) {}
Encoder::~Encoder() = default;
provider::Payload Encoder::call(const provider::MessageKind, const provider::Payload&) {
    return provider::Unavailable{provider::Error::Unavailable,
                                 "This build has no macOS encoder (no FFmpeg package)"};
}
bool hardwareEncodeAvailable() { return false; }

#else

namespace {

using provider::Error;
using provider::Unavailable;

// Canonical records require reduced rationals with a positive denominator.
[[nodiscard]] provider::Rational reducedRational(std::int64_t numerator, std::int64_t denominator) {
    if (denominator < 0) {
        numerator = -numerator;
        denominator = -denominator;
    }
    if (denominator == 0 || numerator == 0)
        return {0, 1};
    const auto magnitude = numerator < 0 ? -numerator : numerator;
    const auto divisor = std::gcd(magnitude, denominator);
    return {numerator / divisor, denominator / divisor};
}

// AVAssetWriter reports completion on the calling thread's run loop. A worker is a plain loop
// with none of its own, so the waits below spin one briefly.
void pumpRunLoop(const std::chrono::milliseconds budget) {
    const auto end = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < end) {
        @autoreleasepool {
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                     beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        }
    }
}

// A writer that has failed never becomes ready again; its error says why.
[[nodiscard]] std::string writerFailure(AVAssetWriter* writer) {
    NSError* error = writer.error;
    if (error == nil)
        return "AVAssetWriter failed";
    NSError* underlying = error.userInfo[NSUnderlyingErrorKey];
    char text[200] = {};
    if (underlying != nil)
        std::snprintf(text, sizeof(text), "AVAssetWriter failed (%s %ld, %s %ld)",
                      error.domain.UTF8String, static_cast<long>(error.code),
                      underlying.domain.UTF8String, static_cast<long>(underlying.code));
    else
        std::snprintf(text, sizeof(text), "AVAssetWriter failed (%s %ld)", error.domain.UTF8String,
                      static_cast<long>(error.code));
    return text;
}

struct Codec final {
    AVVideoCodecType type = nil;
    CMVideoCodecType code = 0;
    // The Y'CbCr samples handed to the encoder: the planar layout the conversion produces and the
    // pixel buffer arrangement of the same samples.
    AVPixelFormat planar = AV_PIX_FMT_NONE;
    OSType buffer = 0;
    bool alpha = false;
    // 4:2:0 at 10 bits: the two-plane buffer holds each sample in the high bits of a 16-bit word.
    bool deep = false;
};

[[nodiscard]] std::optional<Codec> codecFor(const provider::EncodeSettingsV1& settings) {
    Codec codec;
    if (settings.videoCodec == "h264") {
        if (settings.profile != "high")
            return std::nullopt;
        codec.type = AVVideoCodecTypeH264;
        codec.code = kCMVideoCodecType_H264;
        codec.planar = AV_PIX_FMT_YUV420P;
        codec.buffer = settings.colour.range == 2 ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                                                  : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        return codec;
    }
    if (settings.videoCodec == "hevc") {
        // Main is 8-bit and Main 10 is 10-bit, both 4:2:0; the profile asked for is the one
        // written, never the other.
        const bool deep = settings.profile == "main10";
        if (!deep && settings.profile != "main")
            return std::nullopt;
        const bool full = settings.colour.range == 2;
        codec.type = AVVideoCodecTypeHEVC;
        codec.code = kCMVideoCodecType_HEVC;
        codec.planar = deep ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
        codec.buffer = deep   ? (full ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange
                                      : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange)
                       : full ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                              : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        codec.deep = deep;
        return codec;
    }
    if (settings.videoCodec != "prores")
        return std::nullopt;
    const auto& profile = settings.profile;
    if (profile == "4444" || profile == "4444xq") {
        if (profile == "4444") {
            codec.type = AVVideoCodecTypeAppleProRes4444;
            codec.code = kCMVideoCodecType_AppleProRes4444;
        } else if (@available(macOS 15.0, *)) {
            // The 4444 XQ writer arrived in macOS 15; an older system does not offer the profile.
            codec.type = AVVideoCodecTypeAppleProRes4444XQ;
            codec.code = kCMVideoCodecType_AppleProRes4444XQ;
        } else {
            return std::nullopt;
        }
        codec.planar = AV_PIX_FMT_YUVA444P16LE;
        codec.buffer = kCVPixelFormatType_4444AYpCbCr16;
        codec.alpha = true;
        return codec;
    }
    if (profile == "proxy") {
        codec.type = AVVideoCodecTypeAppleProRes422Proxy;
        codec.code = kCMVideoCodecType_AppleProRes422Proxy;
    } else if (profile == "lt") {
        codec.type = AVVideoCodecTypeAppleProRes422LT;
        codec.code = kCMVideoCodecType_AppleProRes422LT;
    } else if (profile == "422") {
        codec.type = AVVideoCodecTypeAppleProRes422;
        codec.code = kCMVideoCodecType_AppleProRes422;
    } else if (profile == "hq") {
        codec.type = AVVideoCodecTypeAppleProRes422HQ;
        codec.code = kCMVideoCodecType_AppleProRes422HQ;
    } else {
        return std::nullopt;
    }
    codec.planar = AV_PIX_FMT_YUV422P16LE;
    codec.buffer = kCVPixelFormatType_422YpCbCr16;
    return codec;
}

// The system encoder that will be asked for by its identifier, so the encoder that ran is known
// and not inferred: a hardware one when the computer has it, a software one otherwise.
struct SystemEncoder final {
    NSString* identifier = nil;
    std::string name;
    bool hardware = false;
};
[[nodiscard]] std::optional<SystemEncoder> systemEncoder(const CMVideoCodecType code) {
    CFArrayRef listed = nullptr;
    if (VTCopyVideoEncoderList(nullptr, &listed) != noErr || listed == nullptr)
        return std::nullopt;
    NSArray* encoders = CFBridgingRelease(listed);
    std::optional<SystemEncoder> chosen;
    for (NSDictionary* entry in encoders) {
        NSNumber* type = entry[(__bridge NSString*)kVTVideoEncoderList_CodecType];
        NSString* identifier = entry[(__bridge NSString*)kVTVideoEncoderList_EncoderID];
        if (type == nil || identifier == nil || type.unsignedIntValue != code)
            continue;
        NSString* name = entry[(__bridge NSString*)kVTVideoEncoderList_EncoderName];
        NSNumber* hardware = entry[(__bridge NSString*)kVTVideoEncoderList_IsHardwareAccelerated];
        SystemEncoder found{identifier, std::string((name != nil ? name : identifier).UTF8String),
                            hardware != nil && hardware.boolValue};
        if (!chosen || (found.hardware && !chosen->hardware))
            chosen = std::move(found);
    }
    return chosen;
}

// 16-bit little-endian words.
[[nodiscard]] std::uint16_t word(const std::uint8_t* at) {
    std::uint16_t value = 0;
    std::memcpy(&value, at, 2);
    return value;
}
void store(std::uint8_t* at, const std::uint16_t value) { std::memcpy(at, &value, 2); }

} // namespace

bool hardwareEncodeAvailable() { return true; }

struct Encoder::State final {
    provider::EncodeSettingsV1 settings;
    std::string path;
    Codec codec;
    SystemEncoder encoder;
    ffmpeg::DeclaredSampling declared;
    SwsContext* scale = nullptr;
    AVFrame* converted = nullptr;
    NSDictionary* colourAttachments = nil;
    AVAssetWriter* writer = nil;
    AVAssetWriterInput* videoInput = nil;
    AVAssetWriterInput* audioInput = nil;
    std::uint64_t frameCount = 0;
    std::uint64_t sampleCount = 0;
    bool finished = false;
    provider::Bytes firstReference;
    provider::Bytes lastReference;
    core::Sha256Hasher audioHasher;
    // AVAssetWriter interleaves its inputs: it stops taking video until audio has caught up, and
    // the reverse. The host sends a frame and then the audio up to it, so whichever the writer is
    // not ready for waits here until the other has been appended.
    std::deque<CMSampleBufferRef> pendingVideo;
    std::deque<CMSampleBufferRef> pendingAudio;
    bool videoClosed = false, audioClosed = false;

    ~State() {
        for (const auto sample : pendingVideo)
            CFRelease(sample);
        for (const auto sample : pendingAudio)
            CFRelease(sample);
        sws_freeContext(scale);
        av_frame_free(&converted);
    }

    // Appends everything the writer is ready for. An input whose data is complete is closed at
    // once, because the writer holds the other input back until it knows no more is coming.
    void drain() {
        for (bool progressed = true; progressed;) {
            progressed = false;
            while (!pendingVideo.empty() && videoInput.isReadyForMoreMediaData) {
                CMSampleBufferRef sample = pendingVideo.front();
                pendingVideo.pop_front();
                const bool appended = [videoInput appendSampleBuffer:sample];
                CFRelease(sample);
                if (!appended)
                    throw Unavailable{Error::Io, writerFailure(writer)};
                progressed = true;
            }
            while (!pendingAudio.empty() && audioInput.isReadyForMoreMediaData) {
                CMSampleBufferRef sample = pendingAudio.front();
                pendingAudio.pop_front();
                const bool appended = [audioInput appendSampleBuffer:sample];
                CFRelease(sample);
                if (!appended)
                    throw Unavailable{Error::Io, writerFailure(writer)};
                progressed = true;
            }
            if (!videoClosed && pendingVideo.empty() && frameCount == settings.frames) {
                [videoInput markAsFinished];
                videoClosed = progressed = true;
            }
            if (audioInput != nil && !audioClosed && pendingAudio.empty() &&
                sampleCount == settings.audioSamples) {
                [audioInput markAsFinished];
                audioClosed = progressed = true;
            }
        }
    }
    // Drains until at most `video` frames and `audio` blocks wait, for as long as `budget`.
    void drainTo(const std::size_t video, const std::size_t audio,
                 const std::chrono::seconds budget, const char* timeout) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (true) {
            drain();
            if (pendingVideo.size() <= video && pendingAudio.size() <= audio)
                return;
            if (writer.status == AVAssetWriterStatusFailed)
                throw Unavailable{Error::Unavailable, writerFailure(writer)};
            if (std::chrono::steady_clock::now() >= deadline)
                throw Unavailable{Error::Timeout, timeout};
            pumpRunLoop(std::chrono::milliseconds(10));
        }
    }

    void begin(const provider::EncodeSettingsV1& requested);
    void frame(const provider::FrameProduct& product);
    void audio(const provider::AudioBlock& block);
    provider::EncodeQcV1 finish();
};

Encoder::Encoder() : state_(std::make_unique<State>()) {}
Encoder::~Encoder() {
    if (!state_->path.empty())
        std::remove(state_->path.c_str());
}

void Encoder::State::begin(const provider::EncodeSettingsV1& requested) {
    if (!provider::valid(requested))
        throw Unavailable{Error::InvalidValue, "Invalid encode settings"};
    if (writer != nil)
        throw Unavailable{Error::UnexpectedMessage, "Encoder already initialized"};
    if (!requested.audioCodec.empty() && requested.audioCodec != "aac" &&
        requested.audioCodec != "pcm_s16le" && requested.audioCodec != "pcm_s24le")
        throw Unavailable{Error::Unavailable, "Audio encoder unavailable"};
    if (requested.container != "mov" && requested.container != "mp4")
        throw Unavailable{Error::Unavailable, "Container unavailable"};
    const auto chosen = codecFor(requested);
    if (!chosen)
        throw Unavailable{Error::Unavailable, "Video encoder or profile unavailable"};
    if (requested.frames == 0)
        throw Unavailable{Error::Unavailable, "Audio-only output unavailable"};
    // H.264 and 4:2:2 ProRes subsample chroma; refuse an extent they cannot code up front rather
    // than fail on the first frame.
    if ((requested.videoCodec == "h264" || requested.videoCodec == "hevc") &&
        ((requested.width % 2U) != 0U || (requested.height % 2U) != 0U))
        throw Unavailable{Error::InvalidValue, requested.videoCodec == "h264"
                                                   ? "H.264 requires even frame dimensions"
                                                   : "HEVC requires even frame dimensions"};
    if (requested.videoCodec == "prores" && !chosen->alpha && (requested.width % 2U) != 0U)
        throw Unavailable{Error::InvalidValue, "ProRes 4:2:2 requires an even width"};
    // ProRes is video range by definition.
    if (requested.videoCodec == "prores" && requested.colour.range == 2)
        throw Unavailable{Error::Unavailable, "ProRes carries video range only"};

    // The tags the export asked for, written as they are (issue #455). A code point the system
    // has no name for, or a matrix the conversion does not implement, is refused, never replaced.
    const auto& tags = requested.colour;
    NSString* primaries =
        (__bridge NSString*)CVColorPrimariesGetStringForIntegerCodePoint(tags.primaries);
    NSString* transfer =
        (__bridge NSString*)CVTransferFunctionGetStringForIntegerCodePoint(tags.transfer);
    NSString* matrix = (__bridge NSString*)CVYCbCrMatrixGetStringForIntegerCodePoint(tags.matrix);
    if (primaries == nil || transfer == nil || matrix == nil || tags.primaries == 2 ||
        tags.transfer == 2 || tags.matrix == 2 ||
        ffmpeg::swsCoefficients(static_cast<AVColorSpace>(tags.matrix)) < 0)
        throw Unavailable{Error::Unavailable, "Color tags unavailable in the macOS encoder"};
    const auto found = systemEncoder(chosen->code);
    if (!found)
        throw Unavailable{Error::Unavailable, "No system encoder for this codec"};

    settings = requested;
    codec = *chosen;
    encoder = *found;
    declared = {static_cast<AVColorSpace>(tags.matrix),
                tags.range == 2 ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG, AVCHROMA_LOC_LEFT};
    // The FFmpeg worker's output conversion: RGBA16 to the stream's own sampling under the
    // declared matrix, range and siting. The system encoder is handed those samples and converts
    // nothing.
    scale = ffmpeg::declaredConversion(static_cast<int>(settings.width),
                                       static_cast<int>(settings.height), AV_PIX_FMT_RGBA64LE,
                                       codec.planar, declared, ffmpeg::kOutputConversionFlags);
    converted = av_frame_alloc();
    if (scale == nullptr || converted == nullptr)
        throw Unavailable{Error::Unavailable, "Output pixel conversion unavailable"};
    converted->format = codec.planar;
    converted->width = static_cast<int>(settings.width);
    converted->height = static_cast<int>(settings.height);
    if (av_frame_get_buffer(converted, 32) < 0)
        throw Unavailable{Error::Oversized, "Encode frame allocation"};
    colourAttachments = @{
        (__bridge NSString*)kCVImageBufferColorPrimariesKey : primaries,
        (__bridge NSString*)kCVImageBufferTransferFunctionKey : transfer,
        (__bridge NSString*)kCVImageBufferYCbCrMatrixKey : matrix,
        (__bridge NSString*)kCVImageBufferChromaLocationTopFieldKey :
            (__bridge NSString*)kCVImageBufferChromaLocation_Left,
    };

    @autoreleasepool {
        NSString* name =
            [NSString stringWithFormat:@"bloom-encode-%@.%@", [NSUUID UUID].UUIDString,
                                       [NSString stringWithUTF8String:settings.container.c_str()]];
        path = std::string([NSTemporaryDirectory() stringByAppendingPathComponent:name].UTF8String);
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        NSError* error = nil;
        writer = [[AVAssetWriter alloc]
            initWithURL:url
               fileType:settings.container == "mp4" ? AVFileTypeMPEG4 : AVFileTypeQuickTimeMovie
                  error:&error];
        if (writer == nil)
            throw Unavailable{Error::Io, "AVAssetWriter could not create the artifact"};
        NSMutableDictionary* video = [NSMutableDictionary dictionary];
        video[AVVideoCodecKey] = codec.type;
        video[AVVideoWidthKey] = @(settings.width);
        video[AVVideoHeightKey] = @(settings.height);
        video[AVVideoColorPropertiesKey] = @{
            AVVideoColorPrimariesKey : primaries,
            AVVideoTransferFunctionKey : transfer,
            AVVideoYCbCrMatrixKey : matrix,
        };
        video[AVVideoEncoderSpecificationKey] =
            @{(__bridge NSString*)kVTVideoEncoderSpecification_EncoderID : encoder.identifier};
        if (settings.videoCodec == "h264") {
            // The requested bit rate, or the FFmpeg worker's default for a hardware encoder.
            const std::uint64_t bitrate = settings.videoBitrateKbps != 0
                                              ? std::uint64_t{settings.videoBitrateKbps} * 1000U
                                              : 30000000U;
            video[AVVideoCompressionPropertiesKey] = @{
                AVVideoProfileLevelKey : AVVideoProfileLevelH264HighAutoLevel,
                AVVideoAverageBitRateKey : @(bitrate),
            };
        } else if (settings.videoCodec == "hevc") {
            // The requested bit rate, or the FFmpeg worker's default for HEVC.
            const std::uint64_t bitrate = settings.videoBitrateKbps != 0
                                              ? std::uint64_t{settings.videoBitrateKbps} * 1000U
                                              : 20000000U;
            video[AVVideoCompressionPropertiesKey] = @{
                AVVideoProfileLevelKey :
                    (__bridge NSString*)(codec.deep ? kVTProfileLevel_HEVC_Main10_AutoLevel
                                                    : kVTProfileLevel_HEVC_Main_AutoLevel),
                AVVideoAverageBitRateKey : @(bitrate),
            };
        }
        // The writer validates its settings by raising; that is a refusal, not a crash.
        @try {
            videoInput = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo
                                                        outputSettings:video];
        } @catch (NSException*) {
            videoInput = nil;
        }
        if (videoInput == nil)
            throw Unavailable{Error::Unavailable, "AVAssetWriter refused the video settings"};
        videoInput.expectsMediaDataInRealTime = NO;
        // Frame times are whole ticks of the frame rate, in the track and in the movie's edit,
        // so no timestamp or duration is rounded.
        videoInput.mediaTimeScale = static_cast<CMTimeScale>(settings.rate.numerator);
        writer.movieTimeScale = static_cast<CMTimeScale>(settings.rate.numerator);
        if (![writer canAddInput:videoInput])
            throw Unavailable{Error::Unavailable, "AVAssetWriter rejected the video input"};
        [writer addInput:videoInput];
        if (!settings.audioCodec.empty()) {
            NSMutableDictionary* sound = [NSMutableDictionary dictionary];
            sound[AVSampleRateKey] = @(settings.sampleRate);
            sound[AVNumberOfChannelsKey] = @(settings.channels);
            if (settings.audioCodec == "aac") {
                sound[AVFormatIDKey] = @(kAudioFormatMPEG4AAC);
                sound[AVEncoderBitRateKey] = @(192000);
            } else {
                sound[AVFormatIDKey] = @(kAudioFormatLinearPCM);
                sound[AVLinearPCMBitDepthKey] = @(settings.audioCodec == "pcm_s16le" ? 16 : 24);
                sound[AVLinearPCMIsFloatKey] = @NO;
                sound[AVLinearPCMIsBigEndianKey] = @NO;
                sound[AVLinearPCMIsNonInterleaved] = @NO;
            }
            @try {
                audioInput = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeAudio
                                                            outputSettings:sound];
            } @catch (NSException*) {
                audioInput = nil;
            }
            if (audioInput == nil || ![writer canAddInput:audioInput])
                throw Unavailable{Error::Unavailable, "AVAssetWriter rejected the audio input"};
            audioInput.expectsMediaDataInRealTime = NO;
            [writer addInput:audioInput];
        }
        if (![writer startWriting])
            throw Unavailable{Error::Unavailable, writerFailure(writer)};
        [writer startSessionAtSourceTime:kCMTimeZero];
    }
}

void Encoder::State::frame(const provider::FrameProduct& product) {
    if (!provider::valid(product) || product.format != provider::PixelFormat::Rgba16 ||
        product.planes.size() != 1 || product.planes[0].width != settings.width ||
        product.planes[0].height != settings.height ||
        product.planes[0].stride != settings.width * 8U || frameCount >= settings.frames ||
        product.pts !=
            reducedRational(static_cast<std::int64_t>(frameCount) * settings.rate.denominator,
                            settings.rate.numerator))
        throw Unavailable{Error::IdentityMismatch, "Frame does not match frozen encode request"};
    const auto& rgba = product.planes[0];
    const auto width = settings.width, height = settings.height;
    const auto* source = reinterpret_cast<const std::uint8_t*>(rgba.bytes.data());
    const std::uint8_t* const sources[4] = {source, nullptr, nullptr, nullptr};
    const int strides[4] = {static_cast<int>(rgba.stride), 0, 0, 0};
    if (av_frame_make_writable(converted) < 0 ||
        sws_scale(scale, sources, strides, 0, static_cast<int>(height), converted->data,
                  converted->linesize) != static_cast<int>(height))
        throw Unavailable{Error::Corrupt, "Output pixel conversion failed"};

    @autoreleasepool {
        CVPixelBufferRef buffer = nullptr;
        // Backed by an IOSurface, which a hardware encoder takes without a copy.
        NSDictionary* backing = @{(__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey : @{}};
        if (CVPixelBufferCreate(kCFAllocatorDefault, width, height, codec.buffer,
                                (__bridge CFDictionaryRef)backing, &buffer) != kCVReturnSuccess)
            buffer = nullptr;
        if (buffer == nullptr || CVPixelBufferGetPixelFormatType(buffer) != codec.buffer ||
            CVPixelBufferLockBaseAddress(buffer, 0) != kCVReturnSuccess) {
            if (buffer != nullptr)
                CVPixelBufferRelease(buffer);
            throw Unavailable{Error::Oversized, "Encode pixel buffer allocation"};
        }
        const auto row = [&](const int plane, const std::uint32_t y) {
            return converted->data[plane] +
                   static_cast<std::ptrdiff_t>(y) * converted->linesize[plane];
        };
        if (codec.buffer == kCVPixelFormatType_422YpCbCr16) {
            // 'v216': Cb Y'0 Cr Y'1, 16-bit little-endian.
            auto* base = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
            const auto stride = CVPixelBufferGetBytesPerRow(buffer);
            for (std::uint32_t y = 0; y < height; ++y) {
                auto* to = base + static_cast<std::size_t>(y) * stride;
                const auto *luma = row(0, y), *blue = row(1, y), *red = row(2, y);
                for (std::uint32_t x = 0; x < width / 2U; ++x) {
                    store(to + x * 8U, word(blue + x * 2U));
                    store(to + x * 8U + 2U, word(luma + x * 4U));
                    store(to + x * 8U + 4U, word(red + x * 2U));
                    store(to + x * 8U + 6U, word(luma + x * 4U + 2U));
                }
            }
        } else if (codec.buffer == kCVPixelFormatType_4444AYpCbCr16) {
            // 'y416': A Y' Cb Cr, 16-bit little-endian. Alpha is never matrixed or resampled; it
            // is the frame's own 16-bit alpha.
            auto* base = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
            const auto stride = CVPixelBufferGetBytesPerRow(buffer);
            for (std::uint32_t y = 0; y < height; ++y) {
                auto* to = base + static_cast<std::size_t>(y) * stride;
                const auto* straight = source + static_cast<std::size_t>(y) * rgba.stride;
                const auto *luma = row(0, y), *blue = row(1, y), *red = row(2, y);
                for (std::uint32_t x = 0; x < width; ++x) {
                    store(to + x * 8U, word(straight + x * 8U + 6U));
                    store(to + x * 8U + 2U, word(luma + x * 2U));
                    store(to + x * 8U + 4U, word(blue + x * 2U));
                    store(to + x * 8U + 6U, word(red + x * 2U));
                }
            }
        } else if (codec.deep) {
            // 10-bit 4:2:0: a luma plane and an interleaved Cb, Cr plane of 16-bit little-endian
            // words, each sample in the word's high ten bits.
            auto* luma = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 0));
            auto* chroma =
                static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 1));
            const auto lumaStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0);
            const auto chromaStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1);
            const auto high = [](const std::uint8_t* at) {
                return static_cast<std::uint16_t>(word(at) << 6U);
            };
            for (std::uint32_t y = 0; y < height; ++y) {
                auto* to = luma + static_cast<std::size_t>(y) * lumaStride;
                const auto* from = row(0, y);
                for (std::uint32_t x = 0; x < width; ++x)
                    store(to + x * 2U, high(from + x * 2U));
            }
            for (std::uint32_t y = 0; y < height / 2U; ++y) {
                auto* to = chroma + static_cast<std::size_t>(y) * chromaStride;
                const auto *blue = row(1, y), *red = row(2, y);
                for (std::uint32_t x = 0; x < width / 2U; ++x) {
                    store(to + x * 4U, high(blue + x * 2U));
                    store(to + x * 4U + 2U, high(red + x * 2U));
                }
            }
        } else {
            // 8-bit 4:2:0: a luma plane and an interleaved Cb, Cr plane.
            auto* luma = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 0));
            auto* chroma =
                static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 1));
            const auto lumaStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0);
            const auto chromaStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1);
            for (std::uint32_t y = 0; y < height; ++y)
                std::memcpy(luma + static_cast<std::size_t>(y) * lumaStride, row(0, y), width);
            for (std::uint32_t y = 0; y < height / 2U; ++y) {
                auto* to = chroma + static_cast<std::size_t>(y) * chromaStride;
                const auto *blue = row(1, y), *red = row(2, y);
                for (std::uint32_t x = 0; x < width / 2U; ++x) {
                    to[x * 2U] = blue[x];
                    to[x * 2U + 1U] = red[x];
                }
            }
        }
        CVPixelBufferUnlockBaseAddress(buffer, 0);
        CVBufferSetAttachments(buffer, (__bridge CFDictionaryRef)colourAttachments,
                               kCVAttachmentMode_ShouldPropagate);
        // Each frame carries its own duration, one frame period, so the last frame's (and a
        // single frame's) is stated and not left to the writer.
        CMVideoFormatDescriptionRef description = nullptr;
        CMSampleBufferRef sample = nullptr;
        const CMSampleTimingInfo timing{
            CMTimeMake(settings.rate.denominator,
                       static_cast<std::int32_t>(settings.rate.numerator)),
            CMTimeMake(static_cast<std::int64_t>(frameCount) * settings.rate.denominator,
                       static_cast<std::int32_t>(settings.rate.numerator)),
            kCMTimeInvalid};
        const bool made = CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, buffer,
                                                                       &description) == noErr &&
                          CMSampleBufferCreateReadyWithImageBuffer(
                              kCFAllocatorDefault, buffer, description, &timing, &sample) == noErr;
        if (description != nullptr)
            CFRelease(description);
        CVPixelBufferRelease(buffer);
        if (!made || sample == nullptr)
            throw Unavailable{Error::Oversized, "Encode sample buffer allocation"};
        pendingVideo.push_back(sample);
    }
    if (frameCount == 0)
        firstReference = rgba.bytes;
    if (frameCount + 1 == settings.frames)
        lastReference = rgba.bytes;
    ++frameCount;
    // With audio to come, frames may wait for it: as many as fit in 256 MiB, and at least one.
    // Without audio nothing else can make the writer ready, so none waits.
    const std::size_t frameBytes = static_cast<std::size_t>(width) * height * 8U;
    const std::size_t waiting =
        audioInput != nil && !audioClosed
            ? std::max<std::size_t>(1, (std::size_t{256} << 20U) / frameBytes)
            : 0;
    drainTo(waiting, std::numeric_limits<std::size_t>::max(), std::chrono::seconds(15),
            "AVAssetWriter input never became ready");
}

void Encoder::State::audio(const provider::AudioBlock& block) {
    if (!provider::valid(block) || audioInput == nil || block.sampleRate != settings.sampleRate ||
        block.channels.size() != settings.channels ||
        block.channels[0].size() > settings.audioSamples - sampleCount ||
        block.pts != reducedRational(static_cast<std::int64_t>(sampleCount),
                                     static_cast<std::int64_t>(settings.sampleRate)))
        throw Unavailable{Error::IdentityMismatch, "Audio does not match frozen encode request"};
    // Interleaved float; the writer converts to the output's PCM depth or encodes AAC itself.
    const auto channels = static_cast<std::uint32_t>(block.channels.size());
    const auto frames = block.channels[0].size();
    if (frames == 0)
        throw Unavailable{Error::InvalidValue, "Empty audio block"};
    std::vector<float> interleaved(frames * channels);
    for (std::size_t frame = 0; frame < frames; ++frame)
        for (std::uint32_t channel = 0; channel < channels; ++channel)
            interleaved[frame * channels + channel] = block.channels[channel][frame];
    AudioStreamBasicDescription format{};
    format.mSampleRate = static_cast<double>(block.sampleRate);
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mBytesPerPacket = format.mBytesPerFrame =
        channels * static_cast<std::uint32_t>(sizeof(float));
    format.mFramesPerPacket = 1;
    format.mChannelsPerFrame = channels;
    format.mBitsPerChannel = 32;
    const auto bytes = interleaved.size() * sizeof(float);
    CMAudioFormatDescriptionRef description = nullptr;
    CMBlockBufferRef data = nullptr;
    CMSampleBufferRef sample = nullptr;
    const bool made =
        CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &format, 0, nullptr, 0, nullptr,
                                       nullptr, &description) == noErr &&
        CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, bytes, kCFAllocatorDefault,
                                           nullptr, 0, bytes, 0, &data) == kCMBlockBufferNoErr &&
        CMBlockBufferReplaceDataBytes(interleaved.data(), data, 0, bytes) == kCMBlockBufferNoErr &&
        // The block starts at sample frame `sampleCount`; each sample frame lasts one tick of
        // the sample rate.
        CMAudioSampleBufferCreateReadyWithPacketDescriptions(
            kCFAllocatorDefault, data, description, static_cast<CMItemCount>(frames),
            CMTimeMake(static_cast<std::int64_t>(sampleCount),
                       static_cast<std::int32_t>(block.sampleRate)),
            nullptr, &sample) == noErr;
    if (data != nullptr)
        CFRelease(data);
    if (description != nullptr)
        CFRelease(description);
    if (!made || sample == nullptr)
        throw Unavailable{Error::Oversized, "Audio sample buffer allocation"};
    pendingAudio.push_back(sample);
    for (std::size_t index = 0; index < frames; ++index)
        for (const auto& channel : block.channels) {
            const float value = channel[index];
            (void)audioHasher.update(std::as_bytes(std::span(&value, std::size_t{1})));
        }
    sampleCount += frames;
    // Audio blocks are small; a few seconds of them may wait for video.
    drainTo(std::numeric_limits<std::size_t>::max(), videoClosed ? 0 : 256,
            std::chrono::seconds(15), "AVAssetWriter audio input never became ready");
}

provider::EncodeQcV1 Encoder::State::finish() {
    if (finished || frameCount != settings.frames)
        throw Unavailable{Error::InvalidValue, "Incomplete encode input"};
    if (audioInput != nil && sampleCount != settings.audioSamples)
        throw Unavailable{Error::InvalidValue, "Incomplete audio input"};
    @autoreleasepool {
        // Everything has arrived; append what waited and close both inputs.
        drainTo(0, 0, std::chrono::seconds(30), "AVAssetWriter did not finish");
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [writer finishWritingWithCompletionHandler:^{
          dispatch_semaphore_signal(done);
        }];
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (dispatch_semaphore_wait(done, DISPATCH_TIME_NOW) != 0) {
            if (std::chrono::steady_clock::now() >= deadline)
                throw Unavailable{Error::Timeout, "AVAssetWriter did not finish"};
            pumpRunLoop(std::chrono::milliseconds(10));
        }
        if (writer.status != AVAssetWriterStatusCompleted)
            throw Unavailable{Error::Io, writerFailure(writer)};
    }

    // Reopen the artifact with the macOS reader and check what it actually holds. Tolerance
    // profile v3, as in the FFmpeg worker: the reference goes through the sample format and the
    // sampling the reopened stream declares, the maximum is gated after each frame's largest
    // 0.01% of color samples are set aside, and alpha is compared where the profile carries it.
    const auto require = [](const bool condition, const std::string& detail) {
        if (!condition)
            throw Unavailable{Error::Corrupt, detail};
    };
    Reader reader;
    provider::CallRequest call;
    call.capability.role = provider::Role::Probe;
    call.source = path;
    auto described = reader.call(call);
    const auto* probe = std::get_if<provider::ProbeResult>(&described);
    if (probe == nullptr) {
        const auto* error = std::get_if<Unavailable>(&described);
        throw Unavailable{Error::Corrupt, "Reopen could not read the artifact" +
                                              (error ? ": " + error->detail : std::string{})};
    }
    provider::EncodeQcV1 qc;
    qc.bytes = probe->sourceBytes;
    qc.artifact = probe->sourceDigest;
    require(qc.bytes != 0, "Empty encoded artifact");
    require(probe->streams.size() == (audioInput != nil ? 2U : 1U) &&
                probe->streams[0].kind == provider::MediaKind::Video,
            "Reopened stream layout differs");
    const auto& stream = probe->streams[0];
    require(stream.codec == settings.videoCodec, "Reopened video codec differs");
    require(stream.width == settings.width && stream.height == settings.height,
            "Reopened dimensions differ");
    require(stream.frameCount == settings.frames, "Reopened frame count differs");
    // The reader accepts only a regular cadence, so the rate and the count fix every timestamp.
    const auto rate = reducedRational(settings.rate.numerator, settings.rate.denominator);
    require(stream.rate == rate,
            "Reopened video timing differs (rate " + std::to_string(stream.rate.numerator) + "/" +
                std::to_string(stream.rate.denominator) + ", expected " +
                std::to_string(rate.numerator) + "/" + std::to_string(rate.denominator) + ")");
    qc.frames = stream.frameCount;
    qc.colour = stream.colour;
    qc.profile = stream.profile;
    qc.bitDepth = stream.pixelFormat.find("p12") != std::string::npos   ? 12U
                  : stream.pixelFormat.find("p10") != std::string::npos ? 10U
                                                                        : 8U;
    // "<container>/<codec>", the container from the file's own major brand.
    {
        std::array<char, 12> head{};
        std::FILE* file = std::fopen(path.c_str(), "rb");
        const bool read = file != nullptr && std::fread(head.data(), 1, head.size(), file) == 12;
        if (file != nullptr)
            std::fclose(file);
        require(read && std::memcmp(head.data() + 4, "ftyp", 4) == 0,
                "Reopened container is unreadable");
        qc.container = std::string(std::memcmp(head.data() + 8, "qt  ", 4) == 0 ? "mov" : "mp4") +
                       "/" + stream.codec;
    }
    qc.encoder = encoder.name + (encoder.hardware ? " (hardware)" : " (software)");
    if (audioInput != nil) {
        const auto& sound = probe->streams[1];
        require(sound.kind == provider::MediaKind::Audio &&
                    sound.sampleRate == settings.sampleRate &&
                    sound.channelLayout.size() == settings.channels &&
                    sound.codec == settings.audioCodec,
                "Reopened audio layout differs");
        qc.audioSamples = sampleCount;
        qc.audio = audioHasher.finalize();
    }

    for (const bool last : {false, true}) {
        if (last && settings.frames == 1)
            break;
        call.capability.role = provider::Role::VideoDecode;
        call.capability.codec = stream.codec;
        call.stream = 0;
        call.frame = last ? settings.frames - 1 : 0;
        auto decoded = reader.planes(call);
        const auto* planes = std::get_if<DecodedPlanes>(&decoded);
        if (planes == nullptr)
            throw Unavailable{Error::Corrupt, "Reopen could not decode the artifact: " +
                                                  std::get<Unavailable>(decoded).detail};
        const auto format = static_cast<AVPixelFormat>(ffmpegPixelFormat(planes->frame));
        require(format != AV_PIX_FMT_NONE, "Reopened sample layout is not comparable");
        // The codec's own subsampling: the decoded layout must decimate chroma as the encoder's
        // input did.
        const auto expected =
            codec.planar == AV_PIX_FMT_YUV420P || codec.planar == AV_PIX_FMT_YUV420P10LE
                ? ChromaSampling::S420
            : codec.planar == AV_PIX_FMT_YUV422P16LE ? ChromaSampling::S422
                                                     : ChromaSampling::S444;
        require(planes->frame.sampling == expected, "Reopened chroma sampling differs");
        AVFrame* view = av_frame_alloc();
        if (view == nullptr)
            throw Unavailable{Error::Oversized, "Reopen allocation"};
        view->format = format;
        view->width = static_cast<int>(planes->frame.width);
        view->height = static_cast<int>(planes->frame.height);
        for (std::size_t index = 0; index < 4; ++index) {
            view->data[index] = const_cast<std::uint8_t*>(planes->frame.data[index]);
            view->linesize[index] = planes->frame.stride[index];
        }
        // What the reopened stream declares, not what was asked for: a wrong tag shows as error.
        const ffmpeg::DeclaredSampling reopened{
            static_cast<AVColorSpace>(planes->colour.matrix),
            planes->colour.range == 2 ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG,
            static_cast<AVChromaLocation>(planes->frame.chromaLocation)};
        const auto& reference = last ? lastReference : firstReference;
        auto measure =
            ffmpeg::measureReopen(*view, std::span<const std::byte>(reference), reopened);
        av_frame_free(&view);
        require(measure.failure.empty(), measure.failure);
        if (codec.alpha) {
            // The system encoder writes a 4444 stream without an alpha channel when every frame
            // it was given is opaque. Such a stream decodes opaque, so it is compared with an
            // opaque picture: the reference's own alpha has to be opaque within the bound.
            std::uint32_t alphaError = measure.alphaMaximum;
            if (!planes->frame.alpha) {
                alphaError = 0;
                for (std::size_t offset = 6; offset + 1 < reference.size(); offset += 8)
                    alphaError = std::max<std::uint32_t>(
                        alphaError,
                        65535U -
                            word(reinterpret_cast<const std::uint8_t*>(reference.data()) + offset));
            }
            require(alphaError <= provider::kVideoToleranceAlphaMaximum,
                    "Reopened alpha exceeds tolerance (max=" + std::to_string(alphaError) + ")");
        }
        qc.maximumError = std::max(qc.maximumError, measure.maximum);
        qc.percentileMaximumError = std::max(qc.percentileMaximumError, measure.percentileMaximum);
        if (measure.count != 0)
            qc.meanError = std::max(
                qc.meanError,
                static_cast<std::uint32_t>((measure.sum + measure.count - 1) / measure.count));
        const auto digest = provider::digestBytes(measure.rgba);
        if (!last)
            qc.firstFrame = digest;
        qc.lastFrame = digest;
    }
    require(qc.percentileMaximumError <= provider::kVideoTolerancePercentileMaximum &&
                qc.meanError <= provider::kVideoToleranceMeanCeiling,
            "Reopened pixels exceed tolerance profile v3 (max=" + std::to_string(qc.maximumError) +
                " p99.99max=" + std::to_string(qc.percentileMaximumError) +
                " mean=" + std::to_string(qc.meanError) + ")");
    qc.duration =
        reducedRational(static_cast<std::int64_t>(settings.frames) * settings.rate.denominator,
                        settings.rate.numerator);
    require(stream.duration == qc.duration, "Reopened duration differs");
    finished = true;
    return qc;
}

provider::Payload Encoder::call(const provider::MessageKind kind,
                                const provider::Payload& payload) {
    auto& state = *state_;
    try {
        if (kind == provider::MessageKind::EncodeBegin) {
            state.begin(std::get<provider::EncodeSettingsV1>(payload));
            return std::monostate{};
        }
        if (state.writer == nil)
            throw Unavailable{Error::UnexpectedMessage, "Encoder is not initialized"};
        if (kind == provider::MessageKind::Frame) {
            state.frame(std::get<provider::FrameProduct>(payload));
            return std::monostate{};
        }
        if (kind == provider::MessageKind::Audio) {
            state.audio(std::get<provider::AudioBlock>(payload));
            return std::monostate{};
        }
        if (kind == provider::MessageKind::EncodeFinish)
            return state.finish();
        if (kind == provider::MessageKind::EncodeRead) {
            const auto offset = std::get<std::uint64_t>(payload);
            if (!state.finished)
                throw Unavailable{Error::InvalidValue, "Product is not finished"};
            std::FILE* file = std::fopen(state.path.c_str(), "rb");
            if (file == nullptr)
                throw Unavailable{Error::Io, "Product chunk read failed"};
            struct Close final {
                std::FILE* file;
                ~Close() { std::fclose(file); }
            } close{file};
            if (::fseeko(file, 0, SEEK_END) != 0)
                throw Unavailable{Error::Io, "Product chunk read failed"};
            const auto size = static_cast<std::uint64_t>(::ftello(file));
            if (offset >= size)
                throw Unavailable{Error::InvalidValue, "Invalid product chunk request"};
            provider::EncodedChunkV1 chunk;
            chunk.offset = offset;
            chunk.bytes.resize(static_cast<std::size_t>(
                std::min<std::uint64_t>(provider::kEncodeChunkBytes, size - offset)));
            if (::fseeko(file, static_cast<off_t>(offset), SEEK_SET) != 0 ||
                std::fread(chunk.bytes.data(), 1, chunk.bytes.size(), file) != chunk.bytes.size())
                throw Unavailable{Error::Io, "Product chunk read failed"};
            return chunk;
        }
        throw Unavailable{Error::UnexpectedMessage, "Invalid encode operation"};
    } catch (const Unavailable& error) {
        return error;
    } catch (const std::bad_alloc&) {
        return Unavailable{Error::Oversized, "Encode allocation limit"};
    }
}

#endif

} // namespace bloom::media::videotoolbox
