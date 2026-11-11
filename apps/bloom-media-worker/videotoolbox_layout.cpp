// See videotoolbox_layout.hpp.
#include "videotoolbox_layout.hpp"

#include <cstring>
#include <utility>

#if defined(BLOOM_VIDEOTOOLBOX_LAYOUT_CONVERSION)
extern "C" {
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}
#endif

namespace bloom::media::videotoolbox {
namespace {

using provider::Error;
using provider::Unavailable;

// The one layout that crosses as it is (ffmpeg_provider.cpp, frame()): limited-range 8-bit 4:2:0
// whose chroma is co-sited left, which the host reconstructs with its own filter.
[[nodiscard]] bool direct(const PlanarFrame& frame) noexcept {
    return frame.sampling == ChromaSampling::S420 && frame.depth == 8 && !frame.alpha &&
           !frame.fullRange && frame.chromaLocation <= 1;
}

[[nodiscard]] std::uint32_t chromaWidth(const PlanarFrame& frame) noexcept {
    return frame.sampling == ChromaSampling::S444 ? frame.width : (frame.width + 1) / 2;
}
[[nodiscard]] std::uint32_t chromaHeight(const PlanarFrame& frame) noexcept {
    return frame.sampling == ChromaSampling::S420 ? (frame.height + 1) / 2 : frame.height;
}

void copyDirect(const PlanarFrame& frame, provider::FrameProduct& product) {
    product.format = provider::PixelFormat::Yuv420p8;
    product.planes.clear();
    for (std::size_t index = 0; index < 3; ++index) {
        provider::CpuPlane plane;
        plane.width = index == 0 ? frame.width : chromaWidth(frame);
        plane.height = index == 0 ? frame.height : chromaHeight(frame);
        plane.stride = plane.width;
        plane.bytes.resize(static_cast<std::size_t>(plane.stride) * plane.height);
        for (std::uint32_t row = 0; row < plane.height; ++row)
            std::memcpy(plane.bytes.data() + static_cast<std::size_t>(row) * plane.stride,
                        frame.data[index] + static_cast<std::ptrdiff_t>(row) * frame.stride[index],
                        plane.stride);
        product.planes.push_back(std::move(plane));
    }
}

} // namespace

#if defined(BLOOM_VIDEOTOOLBOX_LAYOUT_CONVERSION)

namespace {
// The pixel format the FFmpeg decoder reports for these samples. Its H.264 and HEVC decoders use
// the full-range ("J") names for 8-bit full-range pictures.
[[nodiscard]] AVPixelFormat pixelFormat(const PlanarFrame& frame) noexcept {
    using S = ChromaSampling;
    if (frame.alpha)
        return frame.sampling == S::S444 && frame.depth == 16 ? AV_PIX_FMT_YUVA444P16LE
                                                              : AV_PIX_FMT_NONE;
    if (frame.depth == 8) {
        if (frame.fullRange)
            return frame.sampling == S::S420   ? AV_PIX_FMT_YUVJ420P
                   : frame.sampling == S::S422 ? AV_PIX_FMT_YUVJ422P
                                               : AV_PIX_FMT_YUVJ444P;
        return frame.sampling == S::S420   ? AV_PIX_FMT_YUV420P
               : frame.sampling == S::S422 ? AV_PIX_FMT_YUV422P
                                           : AV_PIX_FMT_YUV444P;
    }
    if (frame.depth == 10)
        return frame.sampling == S::S420   ? AV_PIX_FMT_YUV420P10LE
               : frame.sampling == S::S422 ? AV_PIX_FMT_YUV422P10LE
                                           : AV_PIX_FMT_YUV444P10LE;
    if (frame.depth == 16)
        return frame.sampling == S::S420   ? AV_PIX_FMT_YUV420P16LE
               : frame.sampling == S::S422 ? AV_PIX_FMT_YUV422P16LE
                                           : AV_PIX_FMT_YUV444P16LE;
    return AV_PIX_FMT_NONE;
}
} // namespace

struct LayoutConverter::State final {
    SwsContext* scale = nullptr;
    std::array<int, 5> key{};
    ~State() { sws_freeContext(scale); }
};

bool layoutConversionAvailable() noexcept { return true; }

provider::Result<std::monostate> LayoutConverter::convert(const PlanarFrame& frame,
                                                          provider::FrameProduct& product) {
    if (frame.width == 0 || frame.height == 0)
        return Unavailable{Error::InvalidValue, "Decoded frame has no extent"};
    if (direct(frame)) {
        copyDirect(frame, product);
        return std::monostate{};
    }
    const auto format = pixelFormat(frame);
    const auto* layout = format == AV_PIX_FMT_NONE ? nullptr : av_pix_fmt_desc_get(format);
    if (layout == nullptr)
        return Unavailable{Error::Unavailable, "Decoded sample layout is not converted"};
    const int width = static_cast<int>(frame.width), height = static_cast<int>(frame.height);
    // Unspecified 4:2:0 siting is the H.273/MPEG-2 default: co-sited left, vertically centred.
    const auto siting = frame.chromaLocation == 0
                            ? AVCHROMA_LOC_LEFT
                            : static_cast<AVChromaLocation>(frame.chromaLocation);
    const int fullRange = frame.fullRange ? 1 : 0;
    const std::array<int, 5> key{width, height, static_cast<int>(format), fullRange,
                                 static_cast<int>(siting)};
    auto& state = *state_;
    // Exactly the FFmpeg worker's settings (ffmpeg_provider.cpp, frame()).
    if (state.scale == nullptr || state.key != key) {
        sws_freeContext(state.scale);
        state.scale = sws_alloc_context();
        if (state.scale == nullptr)
            return Unavailable{Error::Oversized, "Pixel layout unavailable"};
        // Siting applies per axis, and only along an axis the layout subsamples.
        int horizontal = 0, vertical = 0;
        if (av_chroma_location_enum_to_pos(&horizontal, &vertical, siting) < 0)
            return Unavailable{Error::Unavailable, "Chroma siting unavailable"};
        if (layout->log2_chroma_w == 0)
            horizontal = 0;
        if (layout->log2_chroma_h == 0)
            vertical = 0;
        auto* options = static_cast<void*>(state.scale);
        if (av_opt_set_int(options, "srcw", width, 0) < 0 ||
            av_opt_set_int(options, "srch", height, 0) < 0 ||
            av_opt_set_int(options, "src_format", format, 0) < 0 ||
            av_opt_set_int(options, "dstw", width, 0) < 0 ||
            av_opt_set_int(options, "dsth", height, 0) < 0 ||
            av_opt_set_int(options, "dst_format", AV_PIX_FMT_YUVA444P16LE, 0) < 0 ||
            av_opt_set_int(options, "sws_flags", SWS_BILINEAR | SWS_ACCURATE_RND | SWS_BITEXACT,
                           0) < 0 ||
            av_opt_set_int(options, "src_h_chr_pos", horizontal, 0) < 0 ||
            av_opt_set_int(options, "src_v_chr_pos", vertical, 0) < 0 ||
            sws_init_context(state.scale, nullptr, nullptr) < 0) {
            sws_freeContext(state.scale);
            state.scale = nullptr;
            return Unavailable{Error::Unavailable, "Pixel layout unavailable"};
        }
        state.key = key;
    }
    const auto* coefficients = sws_getCoefficients(SWS_CS_ITU709);
    if (sws_setColorspaceDetails(state.scale, coefficients, fullRange, coefficients, fullRange, 0,
                                 1 << 16, 1 << 16) < 0)
        return Unavailable{Error::Unavailable, "Pixel range preservation unavailable"};

    product.format = provider::PixelFormat::Yuva444p16;
    product.planes.clear();
    std::array<std::uint8_t*, 4> data{};
    std::array<int, 4> strides{};
    for (std::size_t index = 0; index < 4; ++index) {
        provider::CpuPlane plane;
        plane.width = frame.width;
        plane.height = frame.height;
        plane.stride = plane.width * 2U;
        plane.bytes.resize(static_cast<std::size_t>(plane.stride) * plane.height);
        product.planes.push_back(std::move(plane));
    }
    for (std::size_t index = 0; index < 4; ++index) {
        data[index] = reinterpret_cast<std::uint8_t*>(product.planes[index].bytes.data());
        strides[index] = static_cast<int>(product.planes[index].stride);
    }
    if (sws_scale(state.scale, frame.data.data(), frame.stride.data(), 0, height, data.data(),
                  strides.data()) != height)
        return Unavailable{Error::Corrupt, "Pixel layout conversion failed"};
    return std::monostate{};
}

#else

struct LayoutConverter::State final {};

bool layoutConversionAvailable() noexcept { return false; }

provider::Result<std::monostate> LayoutConverter::convert(const PlanarFrame& frame,
                                                          provider::FrameProduct& product) {
    if (frame.width == 0 || frame.height == 0)
        return Unavailable{Error::InvalidValue, "Decoded frame has no extent"};
    if (!direct(frame))
        return Unavailable{Error::Unavailable,
                           "This build converts only 8-bit 4:2:0 video (no FFmpeg package)"};
    copyDirect(frame, product);
    return std::monostate{};
}

#endif

LayoutConverter::LayoutConverter() : state_(std::make_unique<State>()) {}
LayoutConverter::~LayoutConverter() = default;

} // namespace bloom::media::videotoolbox
