// See videotoolbox_reader.hpp. Compiled with automatic reference counting.
#include "videotoolbox_reader.hpp"

#include "videotoolbox_layout.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreFoundation/CoreFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <bloom/core/sha256.hpp>
#include <bloom/platform/file_snapshot.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#if !defined(__APPLE__)
#error "videotoolbox_reader.mm requires macOS"
#endif
#if !__has_feature(objc_arc)
#error "videotoolbox_reader.mm requires automatic reference counting"
#endif

namespace bloom::media::videotoolbox {
namespace {

using provider::Error;
using provider::Limits;
using provider::Unavailable;

// Every failure in this file is thrown as the typed answer and returned by Reader::call.
[[noreturn]] void fail(const Error error, std::string detail) {
    throw Unavailable{error, std::move(detail)};
}
void require(const bool condition, const char* detail, const Error error = Error::Corrupt) {
    if (!condition)
        fail(error, detail);
}
// A source this reader hands to the FFmpeg worker (see the header): Unavailable is a refusal the
// provider chain falls back from, and the sentence is what provenance records.
[[noreturn]] void refuse(const std::string& reason) {
    fail(Error::Unavailable, "The macOS media framework does not read " + reason);
}

[[nodiscard]] provider::Rational reduced(std::int64_t numerator, std::int64_t denominator) {
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

[[nodiscard]] std::string lowered(std::string text) {
    for (auto& character : text)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    return text;
}

// `time` in units of 1/`timescale`, when it is a whole number of them.
[[nodiscard]] std::optional<std::int64_t> exactUnits(const CMTime time,
                                                     const std::int32_t timescale) {
    if (!CMTIME_IS_NUMERIC(time) || time.timescale <= 0 || timescale <= 0)
        return std::nullopt;
    const auto scaled = static_cast<__int128>(time.value) * timescale;
    if (scaled % time.timescale != 0)
        return std::nullopt;
    const auto units = scaled / time.timescale;
    if (units > std::numeric_limits<std::int64_t>::max() ||
        units < std::numeric_limits<std::int64_t>::min())
        return std::nullopt;
    return static_cast<std::int64_t>(units);
}

// ---------------------------------------------------------------------------------------------
// The source bytes.

class Bytes final {
  public:
    // The host's snapshot (descriptor 4) when `snapshotBytes` is not zero, the path otherwise.
    Bytes(const std::string& path, const std::uint64_t snapshotBytes) {
        if (snapshotBytes != 0) {
            auto opened = platform::WorkerSnapshotInput::open(snapshotBytes);
            auto* input = std::get_if<std::unique_ptr<platform::WorkerSnapshotInput>>(&opened);
            require(input != nullptr, "The captured source is not available to the worker",
                    Error::IdentityMismatch);
            snapshot_ = std::move(*input);
            // A second descriptor of the same open file, for the media framework to open by its
            // descriptor path. It is never closed-on-exec'd away: this process starts no other.
            descriptor_ = ::fcntl(4, F_DUPFD_CLOEXEC, 5);
            size_ = snapshotBytes;
        } else {
            descriptor_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
            struct stat status{};
            require(descriptor_ >= 0 && ::fstat(descriptor_, &status) == 0 &&
                        S_ISREG(status.st_mode) && status.st_size > 0,
                    "Media file unavailable", Error::Io);
            size_ = static_cast<std::uint64_t>(status.st_size);
        }
        require(descriptor_ >= 0, "Media file unavailable", Error::Io);
    }
    ~Bytes() {
        if (descriptor_ >= 0)
            ::close(descriptor_);
    }
    Bytes(const Bytes&) = delete;
    Bytes& operator=(const Bytes&) = delete;

    [[nodiscard]] bool snapshot() const noexcept { return snapshot_ != nullptr; }
    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] int descriptor() const noexcept { return descriptor_; }

    void read(const std::uint64_t offset, const std::span<std::byte> bytes) const {
        require(offset <= size_ && bytes.size() <= size_ - offset, "Sample lies outside the file");
        std::size_t done = 0;
        while (done < bytes.size()) {
            const auto count = ::pread(descriptor_, bytes.data() + done, bytes.size() - done,
                                       static_cast<off_t>(offset + done));
            if (count < 0 && errno == EINTR)
                continue;
            require(count > 0, "Media file read failed", Error::Io);
            done += static_cast<std::size_t>(count);
        }
    }

    [[nodiscard]] provider::Digest digest() const {
        core::Sha256Hasher hasher;
        std::vector<std::byte> buffer(std::size_t{1} << 20U);
        for (std::uint64_t offset = 0; offset < size_;) {
            const auto count =
                static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size_ - offset));
            read(offset, std::span(buffer.data(), count));
            require(hasher.update(std::span<const std::byte>(buffer.data(), count)),
                    "Source digest failed", Error::Io);
            offset += count;
        }
        return hasher.finalize();
    }

  private:
    std::unique_ptr<platform::WorkerSnapshotInput> snapshot_;
    int descriptor_ = -1;
    std::uint64_t size_ = 0;
};

// ---------------------------------------------------------------------------------------------
// Bitstream facts AVFoundation does not report as properties.

// Reads an H.264 sequence parameter set far enough for the profile and the sample layout.
struct H264Layout final {
    unsigned profile = 0;
    bool constrained = false;
    unsigned chroma = 1; // chroma_format_idc: 1 is 4:2:0, 2 is 4:2:2, 3 is 4:4:4
    unsigned depth = 8;
    // frame_mbs_only_flag: every picture is a frame, none a field pair.
    bool progressive = true;
    // video_full_range_flag, when the stream signals its video type at all.
    std::optional<bool> fullRange;
};
[[nodiscard]] std::optional<H264Layout> h264Layout(const std::uint8_t* nal,
                                                   const std::size_t size) {
    if (nal == nullptr || size < 5)
        return std::nullopt;
    // Drop the NAL header and the emulation prevention bytes.
    std::vector<std::uint8_t> payload;
    payload.reserve(size);
    for (std::size_t index = 1; index < size; ++index) {
        if (index >= 3 && nal[index] == 3 && nal[index - 1] == 0 && nal[index - 2] == 0 &&
            payload.size() >= 2)
            continue;
        payload.push_back(nal[index]);
    }
    std::size_t bit = 0;
    const auto take = [&](const unsigned count) -> std::optional<std::uint32_t> {
        std::uint32_t value = 0;
        for (unsigned index = 0; index < count; ++index, ++bit) {
            if (bit / 8 >= payload.size())
                return std::nullopt;
            value = (value << 1U) | ((payload[bit / 8] >> (7U - bit % 8U)) & 1U);
        }
        return value;
    };
    const auto exponential = [&]() -> std::optional<std::uint32_t> {
        unsigned zeros = 0;
        while (true) {
            const auto next = take(1);
            if (!next)
                return std::nullopt;
            if (*next == 1)
                break;
            if (++zeros > 31)
                return std::nullopt;
        }
        const auto rest = take(zeros);
        if (!rest)
            return std::nullopt;
        return (std::uint32_t{1} << zeros) - 1U + *rest;
    };
    H264Layout layout;
    const auto profile = take(8), flags = take(8), level = take(8);
    if (!profile || !flags || !level || !exponential())
        return std::nullopt;
    layout.profile = *profile;
    layout.constrained = (*flags & 0x40U) != 0;
    static constexpr std::array<unsigned, 13> kHigh{100, 110, 122, 244, 44,  83, 86,
                                                    118, 128, 138, 139, 134, 135};
    if (std::ranges::find(kHigh, layout.profile) != kHigh.end()) {
        const auto chroma = exponential();
        if (!chroma || *chroma > 3)
            return std::nullopt;
        layout.chroma = *chroma;
        if (layout.chroma == 3 && !take(1))
            return std::nullopt;
        const auto luma = exponential(), chromaDepth = exponential();
        if (!luma || !chromaDepth || *luma > 8 || *chromaDepth != *luma)
            return std::nullopt;
        layout.depth = 8U + *luma;
        const auto bypass = take(1), matrices = take(1);
        if (!bypass || !matrices)
            return std::nullopt;
        if (*matrices == 1)
            for (unsigned list = 0; list < (layout.chroma == 3 ? 12U : 8U); ++list) {
                const auto present = take(1);
                if (!present)
                    return std::nullopt;
                if (*present == 0)
                    continue;
                int last = 8, next = 8;
                for (unsigned entry = 0; entry < (list < 6 ? 16U : 64U); ++entry) {
                    if (next != 0) {
                        const auto coded = exponential();
                        if (!coded)
                            return std::nullopt;
                        // Signed Exp-Golomb: odd codes are positive, even ones negative.
                        const int delta = (*coded & 1U) != 0 ? static_cast<int>((*coded + 1U) / 2U)
                                                             : -static_cast<int>(*coded / 2U);
                        next = (last + delta + 256) % 256;
                    }
                    last = next == 0 ? last : next;
                }
            }
    }
    // The rest of the parameter set up to the video usability information, for the one fact
    // taken from it: whether the stream declares its range. (A stream that does not is reported
    // with an undeclared range, as the FFmpeg worker reports it; the media framework's own
    // description says "video range" for both.)
    const auto skip = [&](const unsigned count) {
        for (unsigned index = 0; index < count; ++index)
            if (!exponential())
                return false;
        return true;
    };
    const auto frameNumber = exponential(), orderType = exponential();
    if (!frameNumber || !orderType)
        return std::nullopt;
    if (*orderType == 0) {
        if (!skip(1))
            return std::nullopt;
    } else if (*orderType == 1) {
        if (!take(1) || !skip(2))
            return std::nullopt;
        const auto cycle = exponential();
        if (!cycle || *cycle > 255 || !skip(*cycle))
            return std::nullopt;
    }
    if (!skip(1) || !take(1) || !skip(2))
        return std::nullopt;
    const auto frames = take(1);
    if (!frames || (*frames == 0 && !take(1)) || !take(1))
        return std::nullopt;
    layout.progressive = *frames == 1;
    const auto cropping = take(1);
    if (!cropping || (*cropping == 1 && !skip(4)))
        return std::nullopt;
    const auto usability = take(1);
    if (!usability)
        return std::nullopt;
    if (*usability == 0)
        return layout;
    const auto aspect = take(1);
    if (!aspect)
        return std::nullopt;
    if (*aspect == 1) {
        const auto shape = take(8);
        if (!shape || (*shape == 255 && (!take(16) || !take(16))))
            return std::nullopt;
    }
    const auto overscan = take(1);
    if (!overscan || (*overscan == 1 && !take(1)))
        return std::nullopt;
    const auto signal = take(1);
    if (!signal)
        return std::nullopt;
    if (*signal == 1) {
        const auto format = take(3), full = take(1);
        if (!format || !full)
            return std::nullopt;
        layout.fullRange = *full == 1;
    }
    return layout;
}

// The names the FFmpeg worker reports, so both providers describe one stream the same way.
[[nodiscard]] std::string h264ProfileName(const H264Layout& layout) {
    switch (layout.profile) {
    case 66:
        return layout.constrained ? "Constrained Baseline" : "Baseline";
    case 77:
        return "Main";
    case 88:
        return "Extended";
    case 100:
        return "High";
    case 110:
        return "High 10";
    case 122:
        return "High 4:2:2";
    case 244:
        return "High 4:4:4 Predictive";
    default:
        return "unknown";
    }
}

// ---------------------------------------------------------------------------------------------
// Tracks.

struct Sample final {
    std::int64_t pts = 0, dts = 0;
    std::uint64_t offset = 0;
    std::uint32_t size = 0;
    bool sync = false;
};

struct VideoTrack final {
    AVAssetTrack* track = nil;
    // Retained by `track`'s format description array.
    CMVideoFormatDescriptionRef format = nullptr;
    std::int32_t timescale = 0;
    // The media time presented at time zero (one leading trim), in track units.
    std::int64_t start = 0;
    std::vector<Sample> samples;             // decode order
    std::vector<std::uint32_t> presentation; // indices into `samples`, by presentation time
    // What the decoder is expected to return.
    ChromaSampling sampling = ChromaSampling::S420;
    unsigned depth = 8;
    bool alpha = false;
    int chromaLocation = 0;
    provider::ColourTags colour;
};

struct AudioTrack final {
    AVAssetTrack* track = nil;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
};

struct Described final {
    provider::ProbeResult probe;
    // By stream id; a stream that is neither has no entry.
    std::map<std::uint32_t, VideoTrack> video;
    std::map<std::uint32_t, AudioTrack> audio;
};

[[nodiscard]] NSDictionary* extensions(const CMFormatDescriptionRef format) {
    return (__bridge NSDictionary*)CMFormatDescriptionGetExtensions(format);
}

// The H.273 code points of the tags the file carries; 2 (unspecified) for a tag it does not carry
// and 0 for an undeclared range, as the FFmpeg worker reports them.
[[nodiscard]] provider::ColourTags colourTags(const CMFormatDescriptionRef format) {
    NSDictionary* tags = extensions(format);
    provider::ColourTags colour{2, 2, 2, 0};
    if (id value = tags[(__bridge NSString*)kCMFormatDescriptionExtension_ColorPrimaries])
        colour.primaries =
            CVColorPrimariesGetIntegerCodePointForString((__bridge CFStringRef)value);
    if (id value = tags[(__bridge NSString*)kCMFormatDescriptionExtension_TransferFunction])
        colour.transfer =
            CVTransferFunctionGetIntegerCodePointForString((__bridge CFStringRef)value);
    if (id value = tags[(__bridge NSString*)kCMFormatDescriptionExtension_YCbCrMatrix])
        colour.matrix = CVYCbCrMatrixGetIntegerCodePointForString((__bridge CFStringRef)value);
    if (NSNumber* full = tags[(__bridge NSString*)kCMFormatDescriptionExtension_FullRangeVideo])
        colour.range = full.boolValue ? 2 : 1;
    return colour;
}

[[nodiscard]] int chromaLocation(const CMFormatDescriptionRef format) {
    NSString* value = extensions(
        format)[(__bridge NSString*)kCMFormatDescriptionExtension_ChromaLocationTopField];
    if (value == nil)
        return 0;
    const auto is = [&](const CFStringRef name) {
        return [value isEqualToString:(__bridge NSString*)name];
    };
    if (is(kCVImageBufferChromaLocation_Left))
        return 1;
    if (is(kCVImageBufferChromaLocation_Center))
        return 2;
    if (is(kCVImageBufferChromaLocation_TopLeft))
        return 3;
    if (is(kCVImageBufferChromaLocation_Top))
        return 4;
    if (is(kCVImageBufferChromaLocation_BottomLeft))
        return 5;
    if (is(kCVImageBufferChromaLocation_Bottom))
        return 6;
    refuse("this chroma siting");
}

[[nodiscard]] std::string pixelFormatName(const ChromaSampling sampling, const unsigned depth,
                                          const bool alpha, const bool fullRange) {
    const char* layout = sampling == ChromaSampling::S420   ? "420"
                         : sampling == ChromaSampling::S422 ? "422"
                                                            : "444";
    std::string name = alpha ? "yuva" : (fullRange && depth == 8 ? "yuvj" : "yuv");
    name += layout;
    name += "p";
    if (depth > 8)
        name += std::to_string(depth) + "le";
    return name;
}

// The start timecode of a timecode track, written as the FFmpeg worker writes it.
[[nodiscard]] std::string startTimecode(AVAssetTrack* track, const Bytes& bytes) {
    if (track.formatDescriptions.count != 1 || !track.canProvideSampleCursors)
        return {};
    const auto format = (__bridge CMTimeCodeFormatDescriptionRef)track.formatDescriptions[0];
    if (CMFormatDescriptionGetMediaSubType(format) != kCMTimeCodeFormatType_TimeCode32)
        return {};
    AVSampleCursor* cursor = [track makeSampleCursorAtFirstSampleInDecodeOrder];
    if (cursor == nil)
        return {};
    const auto range = cursor.currentSampleStorageRange;
    if (range.length < 4 || range.offset < 0)
        return {};
    std::array<std::byte, 4> raw{};
    bytes.read(static_cast<std::uint64_t>(range.offset), raw);
    std::uint64_t frame = 0;
    for (const auto byte : raw)
        frame = (frame << 8U) | std::to_integer<std::uint64_t>(byte);
    const std::uint64_t quanta = CMTimeCodeFormatDescriptionGetFrameQuanta(format);
    if (quanta == 0)
        return {};
    const bool drop =
        (CMTimeCodeFormatDescriptionGetTimeCodeFlags(format) & kCMTimeCodeFlag_DropFrame) != 0;
    if (drop && (quanta == 30 || quanta == 60)) {
        // Drop-frame counting skips frame numbers, not frames: two (or four) at the start of each
        // minute that is not a multiple of ten.
        const std::uint64_t dropped = quanta == 30 ? 2 : 4;
        const std::uint64_t perTenMinutes = quanta == 30 ? 17982 : 35964;
        const auto tens = frame / perTenMinutes, rest = frame % perTenMinutes;
        frame += 9U * dropped * tens;
        if (rest >= dropped)
            frame += dropped * ((rest - dropped) / (perTenMinutes / 10U));
    }
    const auto frames = frame % quanta, seconds = frame / quanta;
    char text[32] = {};
    std::snprintf(text, sizeof(text), "%02llu:%02llu:%02llu%c%02llu",
                  static_cast<unsigned long long>(seconds / 3600U),
                  static_cast<unsigned long long>((seconds / 60U) % 60U),
                  static_cast<unsigned long long>(seconds % 60U), drop ? ';' : ':',
                  static_cast<unsigned long long>(frames));
    return text;
}

void describeVideo(AVAssetTrack* track, provider::StreamDescriptor& stream, VideoTrack& video) {
    require(track.formatDescriptions.count == 1, "more than one sample description",
            Error::Unavailable);
    const auto format = (__bridge CMVideoFormatDescriptionRef)track.formatDescriptions[0];
    NSDictionary* tags = extensions(format);
    const auto subtype = CMFormatDescriptionGetMediaSubType(format);
    const auto extent = CMVideoFormatDescriptionGetDimensions(format);
    require(extent.width > 0 && extent.height > 0 &&
                extent.width <= static_cast<std::int32_t>(Limits::dimension) &&
                extent.height <= static_cast<std::int32_t>(Limits::dimension) &&
                static_cast<std::uint64_t>(extent.width) *
                        static_cast<std::uint64_t>(extent.height) <=
                    Limits::pixels,
            "Video dimensions exceed limits", Error::Oversized);

    // Pictures the FFmpeg worker turns, resamples or deinterlaces are its to read.
    if (!CGAffineTransformIsIdentity(track.preferredTransform))
        refuse("a rotated or mirrored picture");
    if (NSDictionary* aspect =
            tags[(__bridge NSString*)kCMFormatDescriptionExtension_PixelAspectRatio]) {
        NSNumber* horizontal =
            aspect[(__bridge NSString*)kCMFormatDescriptionKey_PixelAspectRatioHorizontalSpacing];
        NSNumber* vertical =
            aspect[(__bridge NSString*)kCMFormatDescriptionKey_PixelAspectRatioVerticalSpacing];
        if (horizontal != nil && vertical != nil &&
            horizontal.unsignedLongLongValue != vertical.unsignedLongLongValue)
            refuse("a picture with non-square pixels");
    }
    const auto natural = track.naturalSize;
    if (std::llround(natural.width) != extent.width ||
        std::llround(natural.height) != extent.height)
        refuse("a picture with non-square pixels");
    NSNumber* fields = tags[(__bridge NSString*)kCMFormatDescriptionExtension_FieldCount];
    if (fields != nil && fields.intValue != 1)
        refuse("an interlaced picture");

    video.track = track;
    video.format = format;
    video.colour = colourTags(format);
    video.chromaLocation = chromaLocation(format);

    switch (subtype) {
    case kCMVideoCodecType_H264: {
        stream.codec = "h264";
        const std::uint8_t* set = nullptr;
        std::size_t size = 0;
        require(CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, &set, &size, nullptr,
                                                                   nullptr) == noErr,
                "H.264 parameter sets are unavailable");
        const auto layout = h264Layout(set, size);
        require(layout.has_value(), "H.264 sequence parameters are unreadable");
        if (layout->chroma == 0)
            refuse("a monochrome H.264 stream");
        if (layout->depth != 8 && layout->depth != 10)
            refuse("H.264 at this bit depth");
        if (!layout->progressive)
            refuse("an interlaced picture");
        video.sampling = layout->chroma == 1   ? ChromaSampling::S420
                         : layout->chroma == 2 ? ChromaSampling::S422
                                               : ChromaSampling::S444;
        video.depth = layout->depth;
        video.colour.range = !layout->fullRange ? 0 : *layout->fullRange ? 2 : 1;
        stream.profile = h264ProfileName(*layout);
        break;
    }
    case kCMVideoCodecType_HEVC: {
        stream.codec = "hevc";
        NSDictionary* atoms =
            tags[(__bridge NSString*)kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms];
        NSData* record = [atoms isKindOfClass:[NSDictionary class]] ? atoms[@"hvcC"] : nil;
        require([record isKindOfClass:[NSData class]] && record.length >= 19,
                "HEVC configuration is unavailable");
        const auto* raw = static_cast<const std::uint8_t*>(record.bytes);
        const unsigned profile = raw[1] & 0x1FU, chroma = raw[16] & 0x3U;
        const unsigned luma = raw[17] & 0x7U, chromaDepth = raw[18] & 0x7U;
        if (chroma == 0)
            refuse("a monochrome HEVC stream");
        if (luma != chromaDepth || (luma != 0 && luma != 2))
            refuse("HEVC at this bit depth");
        video.sampling = chroma == 1   ? ChromaSampling::S420
                         : chroma == 2 ? ChromaSampling::S422
                                       : ChromaSampling::S444;
        video.depth = 8U + luma;
        stream.profile = profile == 1   ? "Main"
                         : profile == 2 ? "Main 10"
                         : profile == 3 ? "Main Still Picture"
                         : profile == 4 ? "Rext"
                                        : "unknown";
        break;
    }
    case kCMVideoCodecType_AppleProRes422Proxy:
    case kCMVideoCodecType_AppleProRes422LT:
    case kCMVideoCodecType_AppleProRes422:
    case kCMVideoCodecType_AppleProRes422HQ:
    case kCMVideoCodecType_AppleProRes4444:
    case kCMVideoCodecType_AppleProRes4444XQ: {
        stream.codec = "prores";
        const bool four = subtype == kCMVideoCodecType_AppleProRes4444 ||
                          subtype == kCMVideoCodecType_AppleProRes4444XQ;
        video.sampling = four ? ChromaSampling::S444 : ChromaSampling::S422;
        // The decoder returns 16-bit samples for every ProRes flavor.
        video.depth = 16;
        NSNumber* alpha =
            tags[(__bridge NSString*)kCMFormatDescriptionExtension_ContainsAlphaChannel];
        video.alpha = four && alpha != nil && alpha.boolValue;
        // ProRes carries no range flag; it is video range unless the file says otherwise.
        if (video.colour.range == 0)
            video.colour.range = 1;
        stream.profile = subtype == kCMVideoCodecType_AppleProRes422Proxy ? "Proxy"
                         : subtype == kCMVideoCodecType_AppleProRes422LT  ? "LT"
                         : subtype == kCMVideoCodecType_AppleProRes422    ? "Standard"
                         : subtype == kCMVideoCodecType_AppleProRes422HQ  ? "HQ"
                         : subtype == kCMVideoCodecType_AppleProRes4444   ? "4444"
                                                                          : "XQ";
        break;
    }
    default:
        refuse("this video codec");
    }
    const bool fullRange = video.colour.range == 2;
    if (video.sampling != ChromaSampling::S420 && video.depth == 8 && stream.codec == "hevc")
        refuse("8-bit HEVC outside 4:2:0");
    if (!layoutConversionAvailable() && (video.sampling != ChromaSampling::S420 ||
                                         video.depth != 8 || fullRange || video.chromaLocation > 1))
        refuse("this sample layout in a build without the FFmpeg package");

    // The sample table: every sample's times, place in the file and whether a decoder can start
    // there.
    require(track.canProvideSampleCursors, "the track's sample table", Error::Unavailable);
    video.timescale = track.naturalTimeScale;
    require(video.timescale > 0, "Video timebase unavailable");
    AVSampleCursor* cursor = [track makeSampleCursorAtFirstSampleInDecodeOrder];
    require(cursor != nil, "Video track has no samples");
    std::optional<std::int64_t> cadence;
    std::int64_t total = 0;
    do {
        const auto pts = exactUnits(cursor.presentationTimeStamp, video.timescale);
        const auto dts = exactUnits(cursor.decodeTimeStamp, video.timescale);
        const auto duration = exactUnits(cursor.currentSampleDuration, video.timescale);
        const auto range = cursor.currentSampleStorageRange;
        require(pts && dts && duration && *duration > 0 && range.offset >= 0 && range.length > 0 &&
                    range.length <= std::numeric_limits<std::uint32_t>::max(),
                "Video sample table is malformed");
        if (!cadence)
            cadence = *duration;
        else if (*cadence != *duration)
            refuse("a variable frame cadence");
        require(video.samples.size() < Limits::indexEntries, "Index exceeds limits",
                Error::Oversized);
        video.samples.push_back({*pts, *dts, static_cast<std::uint64_t>(range.offset),
                                 static_cast<std::uint32_t>(range.length),
                                 cursor.currentSampleSyncInfo.sampleIsFullSync != NO});
        total += *duration;
    } while ([cursor stepInDecodeOrderByCount:1] == 1);

    video.presentation.resize(video.samples.size());
    std::iota(video.presentation.begin(), video.presentation.end(), 0U);
    std::ranges::sort(video.presentation, [&](const auto left, const auto right) {
        return video.samples[left].pts < video.samples[right].pts;
    });
    for (std::size_t index = 1; index < video.presentation.size(); ++index)
        require(video.samples[video.presentation[index]].pts ==
                    video.samples[video.presentation[index - 1]].pts + *cadence,
                "Video presentation times are not a regular sequence");

    // One edit that presents the whole track from its first frame; anything else changes which
    // frames exist, which the FFmpeg worker resolves.
    NSArray<AVAssetTrackSegment*>* segments = track.segments;
    if (segments.count != 1 || segments[0].empty)
        refuse("a track with more than one edit");
    const auto mapping = segments[0].timeMapping;
    const auto start = exactUnits(mapping.source.start, video.timescale);
    // The edit's length is stored in the movie's time scale, so it is the track's length rounded;
    // an edit that ends after the last frame begins presents every frame.
    const auto lastBegins = CMTimeMake(total - *cadence, video.timescale);
    if (!start || CMTimeCompare(mapping.target.start, kCMTimeZero) != 0 ||
        CMTimeCompare(mapping.source.duration, mapping.target.duration) != 0 ||
        *start != video.samples[video.presentation.front()].pts ||
        CMTimeCompare(mapping.source.duration, lastBegins) <= 0)
        refuse("a track whose edit trims or retimes it");
    video.start = *start;

    stream.kind = provider::MediaKind::Video;
    stream.timebase = {1, video.timescale};
    stream.rate = reduced(video.timescale, *cadence);
    stream.width = static_cast<std::uint32_t>(extent.width);
    stream.height = static_cast<std::uint32_t>(extent.height);
    stream.duration = reduced(total, video.timescale);
    stream.frameCount = video.samples.size();
    // ProRes is described at the depth its profile codes (10 bits for 4:2:2, 12 for 4:4:4:4).
    const unsigned describedDepth = stream.codec == "prores"
                                        ? (video.sampling == ChromaSampling::S444 ? 12U : 10U)
                                        : video.depth;
    stream.pixelFormat = pixelFormatName(video.sampling, describedDepth, video.alpha, fullRange);
    stream.format = video.sampling == ChromaSampling::S420 && video.depth == 8 && !fullRange
                        ? provider::PixelFormat::Yuv420p8
                        : provider::PixelFormat::Yuva444p16;
    stream.colour = video.colour;
    // Interlaced pictures were refused above. The FFmpeg worker reports what its decoder learned
    // from the first pictures: progressive for H.264 and ProRes, nothing for HEVC.
    stream.fieldOrder = stream.codec == "hevc" ? 0 : 1;
}

void describeAudio(AVAssetTrack* track, provider::StreamDescriptor& stream, AudioTrack& audio) {
    require(track.formatDescriptions.count == 1, "more than one sample description",
            Error::Unavailable);
    const auto format = (__bridge CMAudioFormatDescriptionRef)track.formatDescriptions[0];
    const auto* basic = CMAudioFormatDescriptionGetStreamBasicDescription(format);
    require(basic != nullptr && basic->mSampleRate >= 1.0 &&
                basic->mSampleRate <= static_cast<double>(Limits::sampleRate) &&
                basic->mSampleRate == std::floor(basic->mSampleRate),
            "Audio rate exceeds limits", Error::Oversized);
    if (basic->mChannelsPerFrame == 0 || basic->mChannelsPerFrame > 2)
        refuse("audio with more than two channels");
    stream.kind = provider::MediaKind::Audio;
    stream.profile = "unknown";
    if (basic->mFormatID == kAudioFormatMPEG4AAC) {
        stream.codec = "aac";
        stream.profile = "LC";
    } else if (basic->mFormatID == kAudioFormatLinearPCM) {
        const auto flags = basic->mFormatFlags;
        const auto bits = basic->mBitsPerChannel;
        const std::string order = (flags & kAudioFormatFlagIsBigEndian) != 0 ? "be" : "le";
        if ((flags & kAudioFormatFlagIsFloat) != 0 && (bits == 32 || bits == 64))
            stream.codec = "pcm_f" + std::to_string(bits) + order;
        else if ((flags & kAudioFormatFlagIsSignedInteger) != 0 &&
                 (bits == 16 || bits == 24 || bits == 32))
            stream.codec = "pcm_s" + std::to_string(bits) + order;
        else
            refuse("this PCM sample format");
    } else {
        refuse("this audio codec");
    }
    audio.track = track;
    audio.sampleRate = static_cast<std::uint32_t>(basic->mSampleRate);
    audio.channels = basic->mChannelsPerFrame;
    const auto timescale = track.naturalTimeScale;
    stream.timebase = {1, timescale > 0 ? timescale : static_cast<std::int32_t>(audio.sampleRate)};
    stream.rate = {static_cast<std::int64_t>(audio.sampleRate), 1};
    const auto duration = track.timeRange.duration;
    if (CMTIME_IS_NUMERIC(duration) && duration.timescale > 0) {
        stream.duration = reduced(duration.value, duration.timescale);
        // Sample frames, or coded packets of 1024 sample frames for AAC.
        const auto frames = static_cast<std::uint64_t>(
            std::llround(CMTimeGetSeconds(duration) * static_cast<double>(audio.sampleRate)));
        stream.frameCount = stream.codec == "aac" ? (frames + 1023U) / 1024U : frames;
    }
    // AAC: the coded packets of the track's sample table, the encoder's priming packets ahead of
    // the edit among them, which is the FFmpeg reader's count.
    if (stream.codec == "aac")
        if (AVSampleCursor* cursor = [track makeSampleCursorAtFirstSampleInDecodeOrder]) {
            std::uint64_t packets = 1;
            for (std::int64_t stepped = 0; (stepped = [cursor stepInDecodeOrderByCount:4096]) > 0 &&
                                           packets < Limits::indexEntries;)
                packets += static_cast<std::uint64_t>(stepped);
            stream.frameCount = packets;
        }
    stream.sampleRate = audio.sampleRate;
    stream.channelLayout =
        audio.channels == 1 ? std::vector<std::string>{"FC"} : std::vector<std::string>{"FL", "FR"};
}

// ---------------------------------------------------------------------------------------------
// One opened source.

struct Opened final {
    std::string path;
    provider::Digest digest;
    std::unique_ptr<Bytes> bytes;
    AVURLAsset* asset = nil;
    Described described;
};

[[nodiscard]] NSString* mediaType(const std::string& path) {
    const auto dot = path.find_last_of('.');
    const auto extension = dot == std::string::npos ? std::string{} : lowered(path.substr(dot + 1));
    if (extension == "mov" || extension == "qt")
        return @"video/quicktime";
    if (extension == "mp4" || extension == "m4v")
        return @"video/mp4";
    return nil;
}

[[nodiscard]] std::unique_ptr<Opened> open(const provider::CallRequest& request) {
    auto opened = std::make_unique<Opened>();
    opened->path = request.source;
    NSString* type = mediaType(request.source);
    if (type == nil)
        refuse("this container");
    opened->bytes = std::make_unique<Bytes>(request.source, request.snapshotBytes);

    NSURL* url = nil;
    NSDictionary* options = nil;
    if (opened->bytes->snapshot()) {
        // The snapshot has no name. The descriptor path names the open file itself, and the media
        // type has to be stated because there is no file extension to take it from.
        if (@available(macOS 14.0, *)) {
            options = @{AVURLAssetOverrideMIMETypeKey : type};
        } else {
            refuse("a captured source before macOS 14");
        }
        url = [NSURL
            fileURLWithPath:[NSString stringWithFormat:@"/dev/fd/%d", opened->bytes->descriptor()]];
        opened->digest = request.sourceDigest;
    } else {
        url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:request.source.c_str()]];
        opened->digest = opened->bytes->digest();
    }
    opened->asset = [AVURLAsset URLAssetWithURL:url options:options];
    NSArray<AVAssetTrack*>* tracks = opened->asset.tracks;
    if (tracks.count == 0)
        refuse("this file (no track was found)");
    require(tracks.count <= Limits::entries, "Too many streams", Error::Oversized);

    auto& described = opened->described;
    auto& probe = described.probe;
    // One name for the QuickTime and MPEG-4 family, the name the FFmpeg worker reports for it.
    probe.container = "mov,mp4,m4a,3gp,3g2,mj2";
    probe.version = "AVFoundation";
    probe.sourceBytes = opened->bytes->size();
    probe.sourceDigest = opened->digest;
    std::string timecode;
    for (AVAssetTrack* track in tracks)
        if ([track.mediaType isEqualToString:AVMediaTypeTimecode] && timecode.empty())
            timecode = startTimecode(track, *opened->bytes);
    // Stream ids follow the file's track order, as the FFmpeg worker's do, so a source that falls
    // back to it later is addressed by the same ids.
    std::uint32_t id = 0;
    for (AVAssetTrack* track in tracks) {
        provider::StreamDescriptor stream;
        stream.id = id;
        if ([track.mediaType isEqualToString:AVMediaTypeVideo]) {
            stream.timecode = timecode;
            describeVideo(track, stream, described.video[id]);
        } else if ([track.mediaType isEqualToString:AVMediaTypeAudio]) {
            describeAudio(track, stream, described.audio[id]);
        } else {
            stream.kind = provider::MediaKind::Data;
            const bool timecodeTrack = [track.mediaType isEqualToString:AVMediaTypeTimecode];
            stream.codec = timecodeTrack ? "timecode" : "none";
            if (timecodeTrack)
                stream.timecode = timecode;
            stream.profile = "unknown";
            const auto timescale = track.naturalTimeScale;
            stream.timebase = {1, timescale > 0 ? timescale : 1};
            const auto duration = track.timeRange.duration;
            if (CMTIME_IS_NUMERIC(duration) && duration.timescale > 0)
                stream.duration = reduced(duration.value, duration.timescale);
        }
        probe.streams.push_back(std::move(stream));
        ++id;
    }
    if (described.video.empty() && described.audio.empty())
        refuse("this file (no video or audio track)");
    require(provider::valid(probe), "Invalid probe metadata");
    return opened;
}

// ---------------------------------------------------------------------------------------------
// Decoded frames.

using Planes = DecodedPlanes;

// 16-bit words from `source` (`shift` drops the padding below the significant bits).
void copyWords(const std::uint8_t* source, const std::size_t sourceStride, const std::size_t first,
               const std::size_t step, const std::uint32_t width, const std::uint32_t height,
               const unsigned shift, std::vector<std::uint8_t>& destination) {
    destination.resize(static_cast<std::size_t>(width) * height * 2U);
    for (std::uint32_t row = 0; row < height; ++row) {
        const auto* from = source + static_cast<std::size_t>(row) * sourceStride;
        auto* to = destination.data() + static_cast<std::size_t>(row) * width * 2U;
        for (std::uint32_t column = 0; column < width; ++column) {
            std::uint16_t word = 0;
            std::memcpy(&word, from + (first + static_cast<std::size_t>(column) * step) * 2U, 2);
            word = static_cast<std::uint16_t>(word >> shift);
            std::memcpy(to + static_cast<std::size_t>(column) * 2U, &word, 2);
        }
    }
}
void copyBytes(const std::uint8_t* source, const std::size_t sourceStride, const std::size_t first,
               const std::size_t step, const std::uint32_t width, const std::uint32_t height,
               std::vector<std::uint8_t>& destination) {
    destination.resize(static_cast<std::size_t>(width) * height);
    for (std::uint32_t row = 0; row < height; ++row) {
        const auto* from = source + static_cast<std::size_t>(row) * sourceStride;
        auto* to = destination.data() + static_cast<std::size_t>(row) * width;
        for (std::uint32_t column = 0; column < width; ++column)
            to[column] = from[first + static_cast<std::size_t>(column) * step];
    }
}

// The planes of a locked pixel buffer. Every layout the decoders return for the streams this
// reader accepts is listed; any other is refused rather than converted by the framework.
[[nodiscard]] Planes planesOf(const CVPixelBufferRef buffer, const VideoTrack& video) {
    Planes result;
    auto& frame = result.frame;
    frame.width = static_cast<std::uint32_t>(CVPixelBufferGetWidth(buffer));
    frame.height = static_cast<std::uint32_t>(CVPixelBufferGetHeight(buffer));
    frame.chromaLocation = video.chromaLocation;
    const auto type = CVPixelBufferGetPixelFormatType(buffer);
    const auto setPlane = [&](const std::size_t index, const std::uint32_t width,
                              const unsigned bytesPerSample) {
        frame.data[index] = result.storage[index].data();
        frame.stride[index] = static_cast<int>(width * bytesPerSample);
    };
    // Luma in plane 0, interleaved Cb and Cr in plane 1 and, with `alpha`, alpha in plane 2.
    const auto biplanar = [&](const ChromaSampling sampling, const unsigned depth,
                              const bool fullRange, const bool alpha = false) {
        require(CVPixelBufferGetPlaneCount(buffer) == (alpha ? 3U : 2U),
                "Decoded picture has an unexpected layout");
        frame.sampling = sampling;
        frame.depth = depth;
        frame.fullRange = fullRange;
        const auto chromaWidth =
            sampling == ChromaSampling::S444 ? frame.width : (frame.width + 1U) / 2U;
        const auto chromaHeight =
            sampling == ChromaSampling::S420 ? (frame.height + 1U) / 2U : frame.height;
        require(CVPixelBufferGetWidthOfPlane(buffer, 0) == frame.width &&
                    CVPixelBufferGetHeightOfPlane(buffer, 0) == frame.height &&
                    CVPixelBufferGetWidthOfPlane(buffer, 1) == chromaWidth &&
                    CVPixelBufferGetHeightOfPlane(buffer, 1) == chromaHeight,
                "Decoded picture has an unexpected extent");
        const auto* luma =
            static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 0));
        const auto* chroma =
            static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 1));
        const auto lumaStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0);
        const auto chromaStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1);
        require(luma != nullptr && chroma != nullptr, "Decoded picture is not readable");
        if (depth == 8) {
            copyBytes(luma, lumaStride, 0, 1, frame.width, frame.height, result.storage[0]);
            copyBytes(chroma, chromaStride, 0, 2, chromaWidth, chromaHeight, result.storage[1]);
            copyBytes(chroma, chromaStride, 1, 2, chromaWidth, chromaHeight, result.storage[2]);
        } else {
            // The significant bits are the high bits of each 16-bit word.
            const unsigned shift = 16U - depth;
            copyWords(luma, lumaStride, 0, 1, frame.width, frame.height, shift, result.storage[0]);
            copyWords(chroma, chromaStride, 0, 2, chromaWidth, chromaHeight, shift,
                      result.storage[1]);
            copyWords(chroma, chromaStride, 1, 2, chromaWidth, chromaHeight, shift,
                      result.storage[2]);
        }
        const unsigned bytes = depth == 8 ? 1 : 2;
        setPlane(0, frame.width, bytes);
        setPlane(1, chromaWidth, bytes);
        setPlane(2, chromaWidth, bytes);
        if (alpha) {
            require(depth == 16 && CVPixelBufferGetWidthOfPlane(buffer, 2) == frame.width &&
                        CVPixelBufferGetHeightOfPlane(buffer, 2) == frame.height,
                    "Decoded picture has an unexpected extent");
            const auto* coverage =
                static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 2));
            require(coverage != nullptr, "Decoded picture is not readable");
            // A stream that codes no alpha is described without one; its decoded plane is opaque.
            frame.alpha = video.alpha;
            if (frame.alpha) {
                copyWords(coverage, CVPixelBufferGetBytesPerRowOfPlane(buffer, 2), 0, 1,
                          frame.width, frame.height, 0, result.storage[3]);
                setPlane(3, frame.width, 2);
            }
        }
    };
    switch (type) {
    case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
        biplanar(ChromaSampling::S420, 8, false);
        break;
    case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
        biplanar(ChromaSampling::S420, 8, true);
        break;
    case kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange:
        biplanar(ChromaSampling::S422, 8, false);
        break;
    case kCVPixelFormatType_422YpCbCr8BiPlanarFullRange:
        biplanar(ChromaSampling::S422, 8, true);
        break;
    case kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange:
        biplanar(ChromaSampling::S444, 8, false);
        break;
    case kCVPixelFormatType_444YpCbCr8BiPlanarFullRange:
        biplanar(ChromaSampling::S444, 8, true);
        break;
    case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
        biplanar(ChromaSampling::S420, 10, false);
        break;
    case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:
        biplanar(ChromaSampling::S420, 10, true);
        break;
    case kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange:
        biplanar(ChromaSampling::S422, 10, false);
        break;
    case kCVPixelFormatType_422YpCbCr10BiPlanarFullRange:
        biplanar(ChromaSampling::S422, 10, true);
        break;
    case kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange:
        biplanar(ChromaSampling::S444, 10, false);
        break;
    case kCVPixelFormatType_444YpCbCr10BiPlanarFullRange:
        biplanar(ChromaSampling::S444, 10, true);
        break;
    case kCVPixelFormatType_422YpCbCr16BiPlanarVideoRange:
        biplanar(ChromaSampling::S422, 16, false);
        break;
    case kCVPixelFormatType_444YpCbCr16BiPlanarVideoRange:
        biplanar(ChromaSampling::S444, 16, false);
        break;
    case kCVPixelFormatType_444YpCbCr16VideoRange_16A_TriPlanar:
        biplanar(ChromaSampling::S444, 16, false, true);
        break;
    case kCVPixelFormatType_422YpCbCr16: {
        // 'v216': Cb Y'0 Cr Y'1, 16-bit little-endian, video range.
        require(CVPixelBufferGetPlaneCount(buffer) == 0 && frame.width % 2U == 0,
                "Decoded picture has an unexpected layout");
        const auto* base = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
        const auto stride = CVPixelBufferGetBytesPerRow(buffer);
        require(base != nullptr && stride >= static_cast<std::size_t>(frame.width) * 4U,
                "Decoded picture is not readable");
        frame.sampling = ChromaSampling::S422;
        frame.depth = 16;
        const auto half = frame.width / 2U;
        copyWords(base, stride, 1, 2, frame.width, frame.height, 0, result.storage[0]);
        copyWords(base, stride, 0, 4, half, frame.height, 0, result.storage[1]);
        copyWords(base, stride, 2, 4, half, frame.height, 0, result.storage[2]);
        setPlane(0, frame.width, 2);
        setPlane(1, half, 2);
        setPlane(2, half, 2);
        break;
    }
    case kCVPixelFormatType_4444AYpCbCr16: {
        // 'y416': A Y' Cb Cr, 16-bit little-endian, video range Y'CbCr and full range alpha.
        require(CVPixelBufferGetPlaneCount(buffer) == 0,
                "Decoded picture has an unexpected layout");
        const auto* base = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
        const auto stride = CVPixelBufferGetBytesPerRow(buffer);
        require(base != nullptr && stride >= static_cast<std::size_t>(frame.width) * 8U,
                "Decoded picture is not readable");
        frame.sampling = ChromaSampling::S444;
        frame.depth = 16;
        frame.alpha = video.alpha;
        copyWords(base, stride, 1, 4, frame.width, frame.height, 0, result.storage[0]);
        copyWords(base, stride, 2, 4, frame.width, frame.height, 0, result.storage[1]);
        copyWords(base, stride, 3, 4, frame.width, frame.height, 0, result.storage[2]);
        setPlane(0, frame.width, 2);
        setPlane(1, frame.width, 2);
        setPlane(2, frame.width, 2);
        if (frame.alpha) {
            copyWords(base, stride, 0, 4, frame.width, frame.height, 0, result.storage[3]);
            setPlane(3, frame.width, 2);
        }
        break;
    }
    default:
        refuse("the sample layout its decoder returned for this stream");
    }
    require(frame.sampling == video.sampling && frame.depth == video.depth,
            "Decoded picture does not match the stream's declared layout", Error::IdentityMismatch);
    return result;
}

// A VideoToolbox session for one track, and the pictures it has decoded that no request has
// taken yet (a frame is decoded before the frames presented earlier that refer to it).
class Decoder final {
  public:
    explicit Decoder(const std::uint32_t stream, const VideoTrack& video) : stream_(stream) {
        VTDecompressionOutputCallbackRecord callback{&Decoder::decoded, this};
        // The stream's own sampling, depth and range, in a documented arrangement: no sample is
        // resampled or rescaled, only laid out (a hardware decoder's own arrangement of 10-bit
        // samples is a packed one). ProRes decoders are left to return theirs, which is 16-bit.
        NSDictionary* destination = nil;
        if (video.depth != 16) {
            const bool full = video.colour.range == 2;
            using S = ChromaSampling;
            const OSType layout =
                video.depth == 8
                    ? (video.sampling == S::S420
                           ? (full ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                                   : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
                       : video.sampling == S::S422
                           ? (full ? kCVPixelFormatType_422YpCbCr8BiPlanarFullRange
                                   : kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange)
                           : (full ? kCVPixelFormatType_444YpCbCr8BiPlanarFullRange
                                   : kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange))
                    : (video.sampling == S::S420
                           ? (full ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange
                                   : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange)
                       : video.sampling == S::S422
                           ? (full ? kCVPixelFormatType_422YpCbCr10BiPlanarFullRange
                                   : kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange)
                           : (full ? kCVPixelFormatType_444YpCbCr10BiPlanarFullRange
                                   : kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange));
            destination = @{(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey : @(layout)};
        }
        const auto status = VTDecompressionSessionCreate(kCFAllocatorDefault, video.format, nullptr,
                                                         (__bridge CFDictionaryRef)destination,
                                                         &callback, &session_);
        if (status != noErr || session_ == nullptr)
            refuse("this stream (no decoder accepted it)");
    }
    ~Decoder() {
        clear();
        if (session_ != nullptr) {
            VTDecompressionSessionInvalidate(session_);
            CFRelease(session_);
        }
    }
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    [[nodiscard]] std::uint32_t stream() const noexcept { return stream_; }

    // The picture of the sample at `target` in decode order. Caller releases it.
    [[nodiscard]] CVPixelBufferRef picture(const VideoTrack& video, const Bytes& bytes,
                                           const std::uint32_t target) {
        if (const auto found = held_.find(target); found != held_.end()) {
            const auto buffer = found->second;
            held_.erase(found);
            return buffer;
        }
        // Continue from the last decoded sample when the target lies after it and no sync sample
        // lies between; otherwise restart at a sync sample. A picture decoded after a sync
        // sample but presented before it (a leading picture) may refer to the pictures before
        // that sync sample, so it is reached from the sync sample before.
        std::uint32_t sync = target;
        while (sync > 0 && !video.samples[sync].sync)
            --sync;
        while (sync > 0 && video.samples[target].pts < video.samples[sync].pts) {
            --sync;
            while (sync > 0 && !video.samples[sync].sync)
                --sync;
        }
        const bool resume = last_ && *last_ < target && *last_ + 1U >= sync;
        if (!resume)
            clear();
        const std::uint32_t first = resume ? *last_ + 1U : sync;
        for (std::uint32_t index = first; index <= target; ++index) {
            // After a restart, the restart point's own leading pictures have nothing to refer to
            // and nothing later refers to them.
            if (!resume && video.samples[index].pts < video.samples[sync].pts)
                continue;
            decode(video, bytes, index);
        }
        VTDecompressionSessionWaitForAsynchronousFrames(session_);
        const auto found = held_.find(target);
        require(found != held_.end(), "The decoder returned no picture for the frame",
                Error::Corrupt);
        const auto buffer = found->second;
        held_.erase(found);
        // Pictures presented before this one will not be asked for by a forward reader; keep the
        // later ones, within a bound.
        const auto presented = video.samples[target].pts;
        for (auto entry = held_.begin(); entry != held_.end();) {
            if (video.samples[entry->first].pts < presented || held_.size() > kHeld) {
                CFRelease(entry->second);
                entry = held_.erase(entry);
            } else {
                ++entry;
            }
        }
        return buffer;
    }

  private:
    static constexpr std::size_t kHeld = 8;

    static void decoded(void* self, void* sample, const OSStatus status, VTDecodeInfoFlags,
                        const CVImageBufferRef image, CMTime, CMTime) {
        auto& decoder = *static_cast<Decoder*>(self);
        // A sample with no picture is an error only when its picture is the one asked for.
        if (status != noErr || image == nullptr)
            return;
        const auto index =
            static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(sample) - 1U);
        if (const auto found = decoder.held_.find(index); found != decoder.held_.end())
            CFRelease(found->second);
        decoder.held_[index] = CVPixelBufferRetain(image);
    }

    void decode(const VideoTrack& video, const Bytes& bytes, const std::uint32_t index) {
        const auto& sample = video.samples[index];
        auto* memory = static_cast<std::byte*>(std::malloc(sample.size));
        require(memory != nullptr, "Media allocation limit", Error::Oversized);
        CMBlockBufferRef block = nullptr;
        // The block owns `memory` and frees it with the allocator named here.
        if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, memory, sample.size,
                                               kCFAllocatorMalloc, nullptr, 0, sample.size, 0,
                                               &block) != kCMBlockBufferNoErr) {
            std::free(memory);
            fail(Error::Oversized, "Media allocation limit");
        }
        struct Release final {
            CFTypeRef object;
            ~Release() {
                if (object != nullptr)
                    CFRelease(object);
            }
        } releaseBlock{block};
        bytes.read(sample.offset, std::span(memory, sample.size));
        CMSampleTimingInfo timing{kCMTimeInvalid, CMTimeMake(sample.pts, video.timescale),
                                  CMTimeMake(sample.dts, video.timescale)};
        const std::size_t size = sample.size;
        CMSampleBufferRef buffer = nullptr;
        require(CMSampleBufferCreateReady(kCFAllocatorDefault, block, video.format, 1, 1, &timing,
                                          1, &size, &buffer) == noErr &&
                    buffer != nullptr,
                "Media allocation limit", Error::Oversized);
        Release releaseBuffer{buffer};
        VTDecodeInfoFlags info = 0;
        // Flags of zero: the call returns after the picture has been handed to the callback.
        const auto status = VTDecompressionSessionDecodeFrame(
            session_, buffer, 0, reinterpret_cast<void*>(static_cast<std::uintptr_t>(index) + 1U),
            &info);
        last_ = index;
        static_cast<void>(status);
    }

    void clear() {
        for (auto& [index, buffer] : held_)
            CFRelease(buffer);
        held_.clear();
        last_.reset();
    }

    std::uint32_t stream_ = 0;
    VTDecompressionSessionRef session_ = nullptr;
    std::map<std::uint32_t, CVPixelBufferRef> held_;
    std::optional<std::uint32_t> last_;
};

// A float reader over one audio track, kept while requests continue where the last one ended.
struct AudioCursor final {
    std::uint32_t stream = 0;
    AVAssetReader* reader = nil;
    AVAssetReaderTrackOutput* output = nil;
    // The sample frame the next decoded buffer is expected to start at, and decoded sample frames
    // not yet taken (interleaved), starting at `pendingBegin`.
    std::uint64_t next = 0;
    std::vector<float> pending;
    std::uint64_t pendingBegin = 0;
};

} // namespace

struct Reader::State final {
    std::unique_ptr<Opened> opened;
    std::unique_ptr<Decoder> decoder;
    LayoutConverter converter;
    std::optional<AudioCursor> audio;

    Opened& source(const provider::CallRequest& request) {
        if (opened) {
            // A retained worker holds one captured source for its lifetime.
            if (request.snapshotBytes != 0)
                require(opened->bytes->snapshot() && request.sourceDigest == opened->digest &&
                            request.snapshotBytes == opened->bytes->size(),
                        "Retained decoder source generation changed", Error::SourceChanged);
            else if (opened->bytes->snapshot() || opened->path != request.source)
                reset();
        }
        if (!opened)
            opened = open(request);
        return *opened;
    }
    void reset() {
        audio.reset();
        decoder.reset();
        opened.reset();
    }

    DecodedPlanes decode(const provider::CallRequest& request) {
        auto& source = this->source(request);
        const auto found = source.described.video.find(request.stream);
        require(found != source.described.video.end(), "Stream does not exist",
                Error::InvalidValue);
        const auto& track = found->second;
        require(request.capability.codec == source.described.probe.streams[request.stream].codec,
                "Stream codec does not match pinned capability", Error::IdentityMismatch);
        require(request.frame < track.presentation.size(), "Frame lies past the end of the stream",
                Error::InvalidValue);
        const auto target = track.presentation[static_cast<std::size_t>(request.frame)];
        if (!decoder || decoder->stream() != request.stream)
            decoder = std::make_unique<Decoder>(request.stream, track);
        const auto buffer = decoder->picture(track, *source.bytes, target);
        struct Release final {
            CVPixelBufferRef buffer;
            ~Release() {
                CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
                CFRelease(buffer);
            }
        };
        require(CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) ==
                    kCVReturnSuccess,
                "Decoded picture is not readable");
        const Release release{buffer};
        auto decoded = planesOf(buffer, track);
        decoded.pts = reduced(track.samples[target].pts - track.start, track.timescale);
        decoded.colour = track.colour;
        // The decoder's range is the stream's; an undeclared range decodes as limited.
        require(decoded.frame.fullRange == (track.colour.range == 2),
                "Decoded range does not match the stream's declared range",
                Error::IdentityMismatch);
        return decoded;
    }

    provider::Payload video(const provider::CallRequest& request) {
        const auto decoded = decode(request);
        provider::FrameProduct product;
        product.pts = decoded.pts;
        product.colour = decoded.colour;
        auto converted = converter.convert(decoded.frame, product);
        if (const auto* error = std::get_if<Unavailable>(&converted))
            throw *error;
        require(provider::valid(product), "Invalid video product");
        return product;
    }

    provider::Payload sound(const provider::CallRequest& request) {
        auto& source = this->source(request);
        const auto found = source.described.audio.find(request.stream);
        require(found != source.described.audio.end(), "Requested stream is not audio",
                Error::InvalidValue);
        const auto& track = found->second;
        require(request.capability.codec == source.described.probe.streams[request.stream].codec,
                "Stream codec does not match pinned capability", Error::IdentityMismatch);
        require(request.samples > 0 && request.samples <= Limits::audioSamples &&
                    request.frame <=
                        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) -
                            request.samples,
                "Audio request exceeds limits", Error::Oversized);
        const std::size_t channels = track.channels;

        // A request that does not continue the previous one starts a reader at its first sample.
        if (!audio || audio->stream != request.stream || audio->next != request.frame) {
            audio.reset();
            AudioCursor cursor;
            cursor.stream = request.stream;
            NSError* error = nil;
            cursor.reader = [[AVAssetReader alloc] initWithAsset:source.asset error:&error];
            require(cursor.reader != nil, "The audio reader could not open the source", Error::Io);
            NSDictionary* settings = @{
                AVFormatIDKey : @(kAudioFormatLinearPCM),
                AVLinearPCMBitDepthKey : @32,
                AVLinearPCMIsFloatKey : @YES,
                AVLinearPCMIsBigEndianKey : @NO,
                AVLinearPCMIsNonInterleaved : @NO,
                AVSampleRateKey : @(track.sampleRate),
                AVNumberOfChannelsKey : @(track.channels),
            };
            cursor.output = [[AVAssetReaderTrackOutput alloc] initWithTrack:track.track
                                                             outputSettings:settings];
            cursor.output.alwaysCopiesSampleData = NO;
            require([cursor.reader canAddOutput:cursor.output],
                    "The audio reader rejected the track", Error::Unavailable);
            [cursor.reader addOutput:cursor.output];
            cursor.reader.timeRange =
                CMTimeRangeMake(CMTimeMake(static_cast<std::int64_t>(request.frame),
                                           static_cast<std::int32_t>(track.sampleRate)),
                                kCMTimePositiveInfinity);
            require([cursor.reader startReading], "The audio reader did not start", Error::Io);
            cursor.next = request.frame;
            cursor.pendingBegin = request.frame;
            audio = std::move(cursor);
        }
        auto& cursor = *audio;

        provider::AudioBlock block;
        block.pts = reduced(static_cast<std::int64_t>(request.frame), track.sampleRate);
        block.sampleRate = track.sampleRate;
        block.channelLayout = source.described.probe.streams[request.stream].channelLayout;
        block.channels.assign(channels, {});
        for (auto& channel : block.channels)
            channel.reserve(request.samples);
        const auto take = [&](const std::vector<float>& interleaved, const std::uint64_t begin) {
            // Sample frames of `interleaved` (starting at `begin`) that fall inside the block.
            const auto filled = block.channels.front().size();
            const auto wanted = request.frame + filled;
            const auto frames = interleaved.size() / channels;
            if (begin + frames <= wanted)
                return frames;
            require(begin <= wanted, "Audio timestamp gap");
            const auto skip = static_cast<std::size_t>(wanted - begin);
            const auto count = std::min<std::size_t>(frames - skip, request.samples - filled);
            for (std::size_t frame = 0; frame < count; ++frame)
                for (std::size_t channel = 0; channel < channels; ++channel)
                    block.channels[channel].push_back(
                        interleaved[(skip + frame) * channels + channel]);
            return skip + count;
        };
        if (!cursor.pending.empty()) {
            const auto used = take(cursor.pending, cursor.pendingBegin);
            cursor.pending.erase(cursor.pending.begin(),
                                 cursor.pending.begin() +
                                     static_cast<std::ptrdiff_t>(used * channels));
            cursor.pendingBegin += used;
        }
        while (block.channels.front().size() < request.samples) {
            CMSampleBufferRef sample = [cursor.output copyNextSampleBuffer];
            if (sample == nullptr) {
                require(cursor.reader.status != AVAssetReaderStatusFailed,
                        "The audio decoder failed", Error::Corrupt);
                break; // the end of the track
            }
            struct Release final {
                CMSampleBufferRef sample;
                ~Release() { CFRelease(sample); }
            } release{sample};
            const auto frames = static_cast<std::size_t>(CMSampleBufferGetNumSamples(sample));
            CMBlockBufferRef data = CMSampleBufferGetDataBuffer(sample);
            if (frames == 0 || data == nullptr)
                continue;
            const auto begin = exactUnits(CMSampleBufferGetPresentationTimeStamp(sample),
                                          static_cast<std::int32_t>(track.sampleRate));
            require(begin.has_value() && *begin >= 0, "Audio timestamp is unavailable");
            std::vector<float> interleaved(frames * channels);
            require(CMBlockBufferGetDataLength(data) == interleaved.size() * sizeof(float) &&
                        CMBlockBufferCopyDataBytes(data, 0, interleaved.size() * sizeof(float),
                                                   interleaved.data()) == kCMBlockBufferNoErr,
                    "Decoded audio is not readable");
            const auto used = take(interleaved, static_cast<std::uint64_t>(*begin));
            if (used < frames) {
                cursor.pending.assign(interleaved.begin() +
                                          static_cast<std::ptrdiff_t>(used * channels),
                                      interleaved.end());
                cursor.pendingBegin = static_cast<std::uint64_t>(*begin) + used;
            }
        }
        require(!block.channels.front().empty(), "No audio at the requested position",
                Error::InvalidValue);
        cursor.next = request.frame + block.channels.front().size();
        require(provider::valid(block), "Invalid audio product");
        return block;
    }

    provider::Payload index(const provider::CallRequest& request) {
        auto& source = this->source(request);
        provider::DemuxIndex result;
        for (const auto& [stream, track] : source.described.video) {
            // A file may store decode times that run ahead of presentation times (negative
            // composition offsets). They are reported moved back by the largest such lead, so
            // that no frame is decoded after it is presented, which is the FFmpeg worker's rule.
            std::int64_t lead = 0;
            for (const auto& sample : track.samples)
                lead = std::max(lead, sample.dts - sample.pts);
            const auto origin = track.start + lead;
            for (const auto& sample : track.samples) {
                if (!sample.sync)
                    continue;
                require(result.keyframes.size() < Limits::indexEntries, "Index exceeds limits",
                        Error::Oversized);
                result.keyframes.push_back({stream,
                                            reduced(sample.pts - track.start, track.timescale),
                                            reduced(sample.dts - origin, track.timescale)});
            }
        }
        return result;
    }
};

Reader::Reader() : state_(std::make_unique<State>()) {}

provider::Result<DecodedPlanes> Reader::planes(const provider::CallRequest& request) {
    @autoreleasepool {
        try {
            return state_->decode(request);
        } catch (const Unavailable& error) {
            state_->reset();
            return error;
        } catch (const std::bad_alloc&) {
            state_->reset();
            return Unavailable{Error::Oversized, "Media allocation limit"};
        }
    }
}
Reader::~Reader() = default;

provider::Payload Reader::call(const provider::CallRequest& request) {
    @autoreleasepool {
        try {
            switch (request.capability.role) {
            case provider::Role::Probe:
                return state_->source(request).described.probe;
            case provider::Role::DemuxIndex:
                return state_->index(request);
            case provider::Role::VideoDecode:
                return state_->video(request);
            case provider::Role::AudioDecode:
                return state_->sound(request);
            default:
                return Unavailable{Error::Unavailable,
                                   "role is not implemented by the VideoToolbox provider"};
            }
        } catch (const Unavailable& error) {
            state_->reset();
            return error;
        } catch (const std::bad_alloc&) {
            state_->reset();
            return Unavailable{Error::Oversized, "Media allocation limit"};
        }
    }
}

} // namespace bloom::media::videotoolbox
